#include "cinder/app/App.h"
#include "cinder/app/RendererGl.h"
#include "cinder/gl/gl.h"
#include "cinder/Rand.h"

// Settings
#include "SDASettings.h"
// Session
#include "SDASession.h"
// Log
#include "SDALog.h"
// Spout
#include "CiSpoutOut.h"

using namespace ci;
using namespace ci::app;
using namespace std;
using namespace SophiaDigitalArt;

/**
 Particle type holds information for rendering and simulation.
 Used to buffer initial simulation values.
 */
struct Particle
{
	vec3	pos;
	vec3	ppos;
	vec3	home;
	ColorA  color;
	float	damping;
};
// How many particles to create. (600k default)

const int NUM_PARTICLES = 600e3;

class ParticleSphereGPUApp : public App {

public:
	ParticleSphereGPUApp();
	void mouseMove(MouseEvent event) override;
	void mouseDown(MouseEvent event) override;
	void mouseDrag(MouseEvent event) override;
	void mouseUp(MouseEvent event) override;
	void keyDown(KeyEvent event) override;
	void keyUp(KeyEvent event) override;
	void fileDrop(FileDropEvent event) override;
	void update() override;
	void draw() override;
	void cleanup() override;
	void setUIVisibility(bool visible);
private:
	// Settings
	SDASettingsRef					mSDASettings;
	// Session
	SDASessionRef					mSDASession;
	// Log
	SDALogRef						mSDALog;
	// imgui
	float							color[4];
	float							backcolor[4];
	int								playheadPositions[12];
	int								speeds[12];

	float							f = 0.0f;
	char							buf[64];
	unsigned int					i, j;

	bool							mouseGlobal;

	string							mError;
	// fbo
	bool							mIsShutDown;
	Anim<float>						mRenderWindowTimer;
	void							positionRenderWindow();
	bool							mFadeInDelay;
	SpoutOut 						mSpoutOut;
	gl::GlslProgRef					mRenderProg;
	gl::GlslProgRef					mUpdateProg;

	// Descriptions of particle data layout.
	gl::VaoRef						mAttributes[2];
	// Buffers holding raw particle data on GPU.
	gl::VboRef						mParticleBuffer[2];

	// Current source and destination buffers for transform feedback.
	// Source and destination are swapped each frame after update.
	std::uint32_t					mSourceIndex = 0;
	std::uint32_t					mDestinationIndex = 1;

	// Mouse state suitable for passing as uniforms to update program
	bool							mMouseDown = false;
	float							mMouseForce = 0.0f;
	vec3							mMousePos = vec3(0, 0, 0);
	vec2							mRightHandPos;
	ivec2							mLeftHandPos;
	// spheres
	gl::BatchRef					mSphereRight, mSphereLeft;
	CameraPersp						mCamera;
	int								mCameraZoom;
};


ParticleSphereGPUApp::ParticleSphereGPUApp()
	: mSpoutOut("SDAParticlesSphere", app::getWindowSize())
{
	// Settings
	mSDASettings = SDASettings::create();
	// Session
	mSDASession = SDASession::create(mSDASettings);
	//mSDASettings->mCursorVisible = true;
	setUIVisibility(mSDASettings->mCursorVisible);
	mSDASession->getWindowsResolution();

	mouseGlobal = false;
	mFadeInDelay = true;

	// Create initial particle layout.
	vector<Particle> particles;
	particles.assign(NUM_PARTICLES, Particle());
	const float azimuth = 256.0f * M_PI / particles.size();
	const float inclination = M_PI / particles.size();
	const float radius = 180.0f;
	vec3 center = vec3(getWindowCenter() + vec2(0.0f, 40.0f), 0.0f);
	for (int i = 0; i < particles.size(); ++i)
	{	// assign starting values to particles.
		float x = radius * sin(inclination * i) * cos(azimuth * i);
		float y = radius * cos(inclination * i);
		float z = radius * sin(inclination * i) * sin(azimuth * i);

		auto &p = particles.at(i);
		p.pos = center + vec3(x, y, z);
		p.home = p.pos;
		p.ppos = p.home + Rand::randVec3() * 10.0f; // random initial velocity
		p.damping = Rand::randFloat(0.965f, 0.985f);
		p.color = Color(CM_HSV, lmap<float>(i, 0.0f, particles.size(), 0.0f, 0.66f), 1.0f, 1.0f);
	}

	// Create particle buffers on GPU and copy data into the first buffer.
	// Mark as static since we only write from the CPU once.
	mParticleBuffer[mSourceIndex] = gl::Vbo::create(GL_ARRAY_BUFFER, particles.size() * sizeof(Particle), particles.data(), GL_STATIC_DRAW);
	mParticleBuffer[mDestinationIndex] = gl::Vbo::create(GL_ARRAY_BUFFER, particles.size() * sizeof(Particle), nullptr, GL_STATIC_DRAW);

	for (int i = 0; i < 2; ++i)
	{	// Describe the particle layout for OpenGL.
		mAttributes[i] = gl::Vao::create();
		gl::ScopedVao vao(mAttributes[i]);

		// Define attributes as offsets into the bound particle buffer
		gl::ScopedBuffer buffer(mParticleBuffer[i]);
		gl::enableVertexAttribArray(0);
		gl::enableVertexAttribArray(1);
		gl::enableVertexAttribArray(2);
		gl::enableVertexAttribArray(3);
		gl::enableVertexAttribArray(4);
		gl::vertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Particle), (const GLvoid*)offsetof(Particle, pos));
		gl::vertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Particle), (const GLvoid*)offsetof(Particle, color));
		gl::vertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Particle), (const GLvoid*)offsetof(Particle, ppos));
		gl::vertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(Particle), (const GLvoid*)offsetof(Particle, home));
		gl::vertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(Particle), (const GLvoid*)offsetof(Particle, damping));
	}

	// Load our update program.
	// Match up our attribute locations with the description we gave.


	mRenderProg = gl::getStockShader(gl::ShaderDef().color());
	mUpdateProg = gl::GlslProg::create(gl::GlslProg::Format().vertex(loadAsset("particleUpdate.vs"))
		.feedbackFormat(GL_INTERLEAVED_ATTRIBS)
		.feedbackVaryings({ "position", "pposition", "home", "color", "damping" })
		.attribLocation("iPosition", 0)
		.attribLocation("iColor", 1)
		.attribLocation("iPPosition", 2)
		.attribLocation("iHome", 3)
		.attribLocation("iDamping", 4)
	);
	auto lambert = gl::ShaderDef().lambert().color();
	gl::GlslProgRef shader = gl::getStockShader(lambert);

	auto sphereRight = geom::Sphere();
	mSphereRight = gl::Batch::create(sphereRight, shader);
	auto sphereLeft = geom::Sphere();
	mSphereLeft = gl::Batch::create(sphereLeft, shader);
	mCameraZoom = 50;
	mCamera.lookAt(vec3(mCameraZoom, 0, 0), vec3(0));
	// windows
	mIsShutDown = false;
	//mRenderWindowTimer = 0.0f;
	//timeline().apply(&mRenderWindowTimer, 1.0f, 2.0f).finishFn([&] { positionRenderWindow(); });

}
void ParticleSphereGPUApp::positionRenderWindow() {
	mSDASettings->mRenderPosXY = ivec2(mSDASettings->mRenderX, mSDASettings->mRenderY);//20141214 was 0
	setWindowPos(mSDASettings->mRenderX, mSDASettings->mRenderY);
	setWindowSize(mSDASettings->mRenderWidth, mSDASettings->mRenderHeight);
}
void ParticleSphereGPUApp::setUIVisibility(bool visible)
{
	if (visible)
	{
		showCursor();
	}
	else
	{
		hideCursor();
	}
}
void ParticleSphereGPUApp::fileDrop(FileDropEvent event)
{
	mSDASession->fileDrop(event);
}
void ParticleSphereGPUApp::update()
{
	mSDASession->setFloatUniformValueByIndex(mSDASettings->IFPS, getAverageFps());
	mSDASession->update();
	mMouseDown = true;
	mMouseForce = 500.0f;
	float iRHandX = mSDASession->getFloatUniformValueByIndex(mSDASettings->IRHANDX) * getWindowWidth() + getWindowWidth() / 2;
	float iRHandY = mSDASession->getFloatUniformValueByIndex(mSDASettings->IRHANDY) * -1.0f * getWindowHeight() + getWindowHeight() / 2;
	float iLHandX = mSDASession->getFloatUniformValueByIndex(mSDASettings->ILHANDX) * getWindowWidth() + getWindowWidth() / 2;
	float iLHandY = mSDASession->getFloatUniformValueByIndex(mSDASettings->ILHANDY) * -1.0f * getWindowHeight() + getWindowHeight() / 2;
	mMousePos = vec3(iRHandX, iRHandY, 0.0f);
	mRightHandPos = vec2(iRHandX, iRHandY);
	mLeftHandPos = vec2(iLHandX, iLHandY);
	// Update particles on the GPU
	gl::ScopedGlslProg prog(mUpdateProg);
	gl::ScopedState rasterizer(GL_RASTERIZER_DISCARD, true);	// turn off fragment stage
	mUpdateProg->uniform("uMouseForce", mMouseForce);
	mUpdateProg->uniform("uMousePos", mMousePos);

	// Bind the source data (Attributes refer to specific buffers).
	gl::ScopedVao source(mAttributes[mSourceIndex]);
	// Bind destination as buffer base.
	gl::bindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, mParticleBuffer[mDestinationIndex]);
	gl::beginTransformFeedback(GL_POINTS);

	// Draw source into destination, performing our vertex transformations.
	gl::drawArrays(GL_POINTS, 0, NUM_PARTICLES);

	gl::endTransformFeedback();

	// Swap source and destination for next loop
	std::swap(mSourceIndex, mDestinationIndex);

	// Update mouse force.
	if (mMouseDown) {
		mMouseForce = 150.0f;
	}
}
void ParticleSphereGPUApp::cleanup()
{
	if (!mIsShutDown)
	{
		mIsShutDown = true;
		CI_LOG_V("shutdown");
		// save settings
		mSDASettings->save();
		mSDASession->save();
		quit();
	}
}
void ParticleSphereGPUApp::mouseMove(MouseEvent event)
{
	if (!mSDASession->handleMouseMove(event)) {
		// let your application perform its mouseMove handling here
	}
}
void ParticleSphereGPUApp::mouseDown(MouseEvent event)
{
	/*mMouseDown = true;
	mMouseForce = 500.0f;
	mMousePos = vec3(event.getX(), event.getY(), 0.0f);*/

}
void ParticleSphereGPUApp::mouseDrag(MouseEvent event)
{
	//mMousePos = vec3(event.getX(), event.getY(), 0.0f);

}
void ParticleSphereGPUApp::mouseUp(MouseEvent event)
{
	/*mMouseForce = 0.0f;
	mMouseDown = false;*/
}

void ParticleSphereGPUApp::keyDown(KeyEvent event)
{
	if (event.getChar() == '-') {
		++mCameraZoom;
		mCamera.lookAt(vec3(mCameraZoom, 0, 0), vec3());
	}
	if (!mSDASession->handleKeyDown(event)) {
		switch (event.getCode()) {
		case KeyEvent::KEY_ESCAPE:
			// quit the application
			quit();
			break;
		case KeyEvent::KEY_h:
			// mouse cursor and ui visibility
			mSDASettings->mCursorVisible = !mSDASettings->mCursorVisible;
			setUIVisibility(mSDASettings->mCursorVisible);
			break;
		}
	}
}
void ParticleSphereGPUApp::keyUp(KeyEvent event)
{
	if (!mSDASession->handleKeyUp(event)) {
	}
}

void ParticleSphereGPUApp::draw()
{
	gl::clear(Color::black());
	/*if (mFadeInDelay) {
		mSDASettings->iAlpha = 0.0f;
		if (getElapsedFrames() > mSDASession->getFadeInDelay()) {
			mFadeInDelay = false;
			timeline().apply(&mSDASettings->iAlpha, 0.0f, 1.0f, 1.5f, EaseInCubic());
		}
	}


	gl::setMatricesWindow(mSDASettings->mRenderWidth, mSDASettings->mRenderHeight, false);
	gl::draw(mSDASession->getMixTexture(), getWindowBounds());*/

	gl::setMatricesWindowPersp(getWindowSize(), 60.0f, 1.0f, 10000.0f);
	gl::enableDepthRead();
	gl::enableDepthWrite();

	gl::ScopedGlslProg render(mRenderProg);
	gl::ScopedVao vao(mAttributes[mSourceIndex]);
	gl::context()->setDefaultShaderVars();
	gl::drawArrays(GL_POINTS, 0, NUM_PARTICLES);

	// Spout Send
	mSpoutOut.sendViewport();
	gl::enableDepthRead();
	gl::enableDepthWrite();

	// left hand
	ci::gl::pushMatrices();
	gl::setMatrices(mCamera);
	gl::translate(mLeftHandPos);
	mSphereLeft->draw();
	ci::gl::popMatrices();

	gl::drawStrokedCircle(mLeftHandPos, 100);
	// right hand
	ci::gl::pushMatrices();
	gl::setMatrices(mCamera);
	gl::translate(mRightHandPos);
	mSphereRight->draw();
	ci::gl::popMatrices();
	gl::drawSolidRect(Rectf(mRightHandPos - vec2(50), mRightHandPos + vec2(50)));

	getWindow()->setTitle(mSDASettings->sFps + " fps SDAParticleSphere");
}

void prepareSettings(App::Settings *settings)
{
	settings->setConsoleWindowEnabled();
	settings->setWindowSize(1280, 720);
}

CINDER_APP(ParticleSphereGPUApp, RendererGl, prepareSettings)
