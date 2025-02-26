/*=====================================================================
GlWidget.cpp
------------
Copyright Glare Technologies Limited 2018 -
=====================================================================*/
#include "GlWidget.h"


#include "PlayerPhysics.h"
#include "CameraController.h"
#include "MainOptionsDialog.h"
#include "../dll/include/IndigoMesh.h"
#include "../indigo/TextureServer.h"
#include "../indigo/globals.h"
#include "../graphics/Map2D.h"
#include "../graphics/ImageMap.h"
#include "../maths/vec3.h"
#include "../maths/GeometrySampling.h"
#include "../utils/Lock.h"
#include "../utils/Mutex.h"
#include "../utils/Clock.h"
#include "../utils/Timer.h"
#include "../utils/Platform.h"
#include "../utils/FileUtils.h"
#include "../utils/Reference.h"
#include "../utils/StringUtils.h"
#include "../utils/TaskManager.h"
#include <QtGui/QMouseEvent>
#include <QtCore/QSettings>
#include <QtWidgets/QShortcut>
#include <set>
#include <stack>
#include <algorithm>


// Export some symbols to indicate to the system that we want to run on a dedicated GPU if present.
// See https://stackoverflow.com/a/39047129
#if defined(_WIN32)
extern "C"
{
	__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
	__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif


// https://wiki.qt.io/How_to_use_OpenGL_Core_Profile_with_Qt
// https://developer.apple.com/opengl/capabilities/GLInfo_1085_Core.html
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))

static QSurfaceFormat makeFormat()
{
	// We need to request a 'core' profile.  Otherwise on OS X, we get an OpenGL 2.1 interface, whereas we require a v3+ interface.
	QSurfaceFormat format;
	// We need to request version 3.2 (or above?) on OS X, otherwise we get legacy version 2.
#ifdef OSX
	format.setVersion(3, 2);
#endif
	format.setProfile(QSurfaceFormat::CoreProfile);
	format.setSamples(4); // Enable multisampling

	return format;
}

#else

static QGLFormat makeFormat()
{
	// We need to request a 'core' profile.  Otherwise on OS X, we get an OpenGL 2.1 interface, whereas we require a v3+ interface.
	QGLFormat format;
	// We need to request version 3.2 (or above?) on OS X, otherwise we get legacy version 2.
#ifdef OSX
	format.setVersion(3, 2);
#endif
//	format.setVersion(4, 6); // TEMP NEW
	format.setProfile(QGLFormat::CoreProfile);
	format.setSampleBuffers(true); // Enable multisampling

//	format.setSwapInterval(0); // TEMP: turn off vsync

	return format;
}

#endif




GlWidget::GlWidget(QWidget *parent)
:	
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
	QOpenGLWidget(parent),
#else
	QGLWidget(makeFormat(), parent),
#endif
	cam_controller(NULL),
	cam_rot_on_mouse_move_enabled(true),
	cam_move_on_key_input_enabled(true),
	near_draw_dist(0.22f), // As large as possible as we can get without clipping becoming apparent.
	max_draw_dist(1000.f),
	//gamepad(NULL),
	print_output(NULL),
	settings(NULL),
	player_physics(NULL),
	take_map_screenshot(false),
	screenshot_ortho_sensor_width_m(10),
	allow_bindless_textures(true),
	allow_multi_draw_indirect(true)
{
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
	setFormat(makeFormat());
#endif

	viewport_aspect_ratio = 1;

	SHIFT_down = false;
	CTRL_down = false;
	W_down = false;
	A_down = false;
	S_down = false;
	D_down = false;
	space_down = false;
	C_down = false;
	left_down = false;
	right_down = false;
	up_down = false;
	down_down = false;
	B_down = false;

	viewport_w = viewport_h = 100;

	// Needed to get keyboard events.
	setFocusPolicy(Qt::StrongFocus);

	setMouseTracking(true); // Set this so we get mouse move events even when a mouse button is not down.


	gamepad_init_timer = new QTimer(this);
	gamepad_init_timer->setSingleShot(true);
	connect(gamepad_init_timer, SIGNAL(timeout()), this, SLOT(initGamepadsSlot()));
	gamepad_init_timer->start(/*msec=*/500); 

	//// See if we have any attached gamepads
	//QGamepadManager* manager = QGamepadManager::instance();

	//const QList<int> list = manager->connectedGamepads();

	//if(!list.isEmpty())
	//{
	//	gamepad = new QGamepad(list.at(0));

	//	connect(gamepad, SIGNAL(axisLeftXChanged(double)), this, SLOT(gamepadInputSlot()));
	//	connect(gamepad, SIGNAL(axisLeftYChanged(double)), this, SLOT(gamepadInputSlot()));
	//}


	// Create a CTRL+C shortcut just for this widget, so it doesn't interfere with the global CTRL+C shortcut for copying text from the chat etc.
	QShortcut* copy_shortcut = new QShortcut(QKeySequence(tr("Ctrl+C")), this);
	connect(copy_shortcut, SIGNAL(activated()), this, SIGNAL(copyShortcutActivated()));
	copy_shortcut->setContext(Qt::WidgetWithChildrenShortcut); // We only want CTRL+C to work for the graphics view when it has focus.

	QShortcut* paste_shortcut = new QShortcut(QKeySequence(tr("Ctrl+V")), this);
	connect(paste_shortcut, SIGNAL(activated()), this, SIGNAL(pasteShortcutActivated()));
	paste_shortcut->setContext(Qt::WidgetWithChildrenShortcut); // We only want CTRL+V to work for the graphics view when it has focus.
}


void GlWidget::initGamepadsSlot()
{
	// See if we have any attached gamepads
	//QGamepadManager* manager = QGamepadManager::instance();

	//const QList<int> list = manager->connectedGamepads();

	//if(!list.isEmpty())
	//{
	//	gamepad = new QGamepad(list.at(0));

	//	//connect(gamepad, SIGNAL(axisLeftXChanged(double)), this, SLOT(gamepadInputSlot()));
	//	//connect(gamepad, SIGNAL(axisLeftYChanged(double)), this, SLOT(gamepadInputSlot()));
	//}
}


//void GlWidget::gamepadInputSlot()
//{
//
//}


GlWidget::~GlWidget()
{
	opengl_engine = NULL;
}


void GlWidget::shutdown()
{
	opengl_engine = NULL;
}


void GlWidget::setCameraController(CameraController* cam_controller_)
{
	cam_controller = cam_controller_;
}


void GlWidget::setPlayerPhysics(PlayerPhysics* player_physics_)
{
	player_physics = player_physics_;
}


void GlWidget::resizeGL(int width_, int height_)
{
	assert(QGLContext::currentContext() == this->context());

	viewport_w = width_;
	viewport_h = height_;

	viewport_aspect_ratio = (double)width_ / (double)height_;

	this->opengl_engine->setViewportDims(viewport_w, viewport_h);

	this->opengl_engine->setMainViewportDims(viewport_w, viewport_h);

#if QT_VERSION_MAJOR >= 6
	// In Qt6, the GL widget uses a custom framebuffer (defaultFramebufferObject).  We want to make sure we draw to this.
	this->opengl_engine->setTargetFrameBuffer(new FrameBuffer(this->defaultFramebufferObject()));
#endif

	emit viewportResizedSignal(width_, height_);
}


void GlWidget::initializeGL()
{
	assert(QGLContext::currentContext() == this->context()); // "There is no need to call makeCurrent() because this has already been done when this function is called."  (https://doc.qt.io/qt-5/qglwidget.html#initializeGL)

	assert(this->texture_server_ptr);

	bool shadows = true;
	bool use_MSAA = true;
	bool bloom = true;
	if(settings)
	{
		shadows  = settings->value(MainOptionsDialog::shadowsKey(),	/*default val=*/true).toBool();
		use_MSAA = settings->value(MainOptionsDialog::MSAAKey(),	/*default val=*/true).toBool();
		bloom    = settings->value(MainOptionsDialog::BloomKey(),	/*default val=*/true).toBool();
	}

#if OSX
	bloom = false; // use_final_image_buffer seems to crash on Mac, don't use it.
#endif

	// Enable debug output (glDebugMessageCallback) in Debug and RelWithDebugInfo mode, e.g. when BUILD_TESTS is 1.
	// Don't enable in Release mode, in case it has a performance cost.
#if BUILD_TESTS
	const bool enable_debug_outout = true;
#else
	const bool enable_debug_outout = false;
#endif

	OpenGLEngineSettings engine_settings;
	engine_settings.enable_debug_output = enable_debug_outout;
	engine_settings.shadow_mapping = shadows;
	engine_settings.compress_textures = true;
	engine_settings.depth_fog = true;
	//engine_settings.use_final_image_buffer = bloom;
	engine_settings.msaa_samples = use_MSAA ? 4 : -1;
	engine_settings.max_tex_mem_usage = 1536 * 1024 * 1024ull; // Should be large enough that we have some spare room for the LRU texture cache.
	engine_settings.allow_multi_draw_indirect = this->allow_multi_draw_indirect;
	engine_settings.allow_bindless_textures = this->allow_bindless_textures;


	opengl_engine = new OpenGLEngine(engine_settings);


	opengl_engine->initialise(
		//"n:/indigo/trunk/opengl", // data dir
		base_dir_path + "/data", // data dir (should contain 'shaders' and 'gl_data')
		this->texture_server_ptr,
		this->print_output
	);
	if(!opengl_engine->initSucceeded())
	{
		conPrint("opengl_engine init failed: " + opengl_engine->getInitialisationErrorMsg());
		initialisation_error_msg = opengl_engine->getInitialisationErrorMsg();
	}

	if(opengl_engine->initSucceeded())
	{
		try
		{
			opengl_engine->setCirrusTexture(opengl_engine->getTexture(base_dir_path + "/resources/cirrus.exr"));
		}
		catch(glare::Exception& e)
		{
			conPrint("Error: " + e.what());
		}
		
		try
		{
			// opengl_engine->setSnowIceTexture(opengl_engine->getTexture(base_dir_path + "/resources/snow-ice-01-normal.png"));
		}
		catch(glare::Exception& e)
		{
			conPrint("Error: " + e.what());
		}

		if(bloom)
			opengl_engine->getCurrentScene()->bloom_strength = 0.3f;
	}
}


void GlWidget::paintGL()
{
	assert(QGLContext::currentContext() == this->context()); // "There is no need to call makeCurrent() because this has already been done when this function is called."  (https://doc.qt.io/qt-5/qglwidget.html#initializeGL)
	if(opengl_engine.isNull())
		return;

	if(take_map_screenshot)
	{
		const Vec3d cam_pos = cam_controller->getPosition();

		const Matrix4f world_to_camera_space_matrix = Matrix4f::rotationAroundXAxis(Maths::pi_2<float>()) * Matrix4f::translationMatrix(-(cam_pos.toVec4fVector()));

		opengl_engine->setViewportDims(viewport_w, viewport_h);
		opengl_engine->setNearDrawDistance(near_draw_dist);
		opengl_engine->setMaxDrawDistance(max_draw_dist);
		opengl_engine->setDiagonalOrthoCameraTransform(world_to_camera_space_matrix, /*sensor_width*/screenshot_ortho_sensor_width_m, /*render_aspect_ratio=*/1.f);
		//opengl_engine->setOrthoCameraTransform(world_to_camera_space_matrix, /*sensor_width*/screenshot_ortho_sensor_width_m, /*render_aspect_ratio=*/1.f, 0, 0);
		opengl_engine->draw();
		return;
	}


	if(cam_controller)
	{
		// Work out current camera transform
		Vec3d cam_pos, up, forwards, right;
		cam_pos = cam_controller->getPosition();
		cam_controller->getBasis(right, up, forwards);

		const Matrix4f rot = Matrix4f(right.toVec4fVector(), forwards.toVec4fVector(), up.toVec4fVector(), Vec4f(0,0,0,1)).getTranspose();

		Matrix4f world_to_camera_space_matrix;
		rot.rightMultiplyWithTranslationMatrix(-cam_pos.toVec4fVector(), /*result=*/world_to_camera_space_matrix);

		const float sensor_width = sensorWidth();
		const float lens_sensor_dist = lensSensorDist();
		const float render_aspect_ratio = viewport_aspect_ratio;
		opengl_engine->setViewportDims(viewport_w, viewport_h);
		opengl_engine->setNearDrawDistance(near_draw_dist);
		opengl_engine->setMaxDrawDistance(max_draw_dist);
		opengl_engine->setPerspectiveCameraTransform(world_to_camera_space_matrix, sensor_width, lens_sensor_dist, render_aspect_ratio, /*lens shift up=*/0.f, /*lens shift right=*/0.f);
		//opengl_engine->setOrthoCameraTransform(world_to_camera_space_matrix, 1000.f, render_aspect_ratio, /*lens shift up=*/0.f, /*lens shift right=*/0.f);
		opengl_engine->draw();
	}

	//conPrint("FPS: " + doubleToStringNSigFigs(1 / fps_timer.elapsed(), 1));
	//fps_timer.reset();
}


void GlWidget::keyPressEvent(QKeyEvent* e)
{
	emit keyPressed(e);

	// Update our key-state variables, jump if space was pressed.
	if(this->player_physics)
	{
		SHIFT_down = (e->modifiers() & Qt::ShiftModifier);
		CTRL_down  = (e->modifiers() & Qt::ControlModifier);

		if(e->key() == Qt::Key::Key_Space)
		{
			if(cam_move_on_key_input_enabled)
				this->player_physics->processJump(*this->cam_controller, /*cur time=*/Clock::getTimeSinceInit());
			space_down = true;
		}
		else if(e->key() == Qt::Key::Key_W)
		{
			W_down = true;
		}
		else if(e->key() == Qt::Key::Key_S)
		{
			S_down = true;
		}
		else if(e->key() == Qt::Key::Key_A)
		{
			A_down = true;
		}
		if(e->key() == Qt::Key::Key_D)
		{
			D_down = true;
		}
		else if(e->key() == Qt::Key::Key_C)
		{
			C_down = true;
		}
		else if(e->key() == Qt::Key::Key_Left)
		{
			left_down = true;
		}
		else if(e->key() == Qt::Key::Key_Right)
		{
			right_down = true;
		}
		else if(e->key() == Qt::Key::Key_Up)
		{
			up_down = true;
		}
		else if(e->key() == Qt::Key::Key_Down)
		{
			down_down = true;
		}
		else if(e->key() == Qt::Key::Key_B)
		{
			B_down = true;
		}
	}
}


void GlWidget::keyReleaseEvent(QKeyEvent* e)
{
	emit keyReleased(e);

	if(this->player_physics)
	{
		SHIFT_down = (e->modifiers() & Qt::ShiftModifier);
		CTRL_down  = (e->modifiers() & Qt::ControlModifier);

		if(e->key() == Qt::Key::Key_Space)
		{
			space_down = false;
		}
		else if(e->key() == Qt::Key::Key_W)
		{
			W_down = false;
		}
		else if(e->key() == Qt::Key::Key_S)
		{
			S_down = false;
		}
		else if(e->key() == Qt::Key::Key_A)
		{
			A_down = false;
		}
		else if(e->key() == Qt::Key::Key_D)
		{
			D_down = false;
		}
		else if(e->key() == Qt::Key::Key_C)
		{
			C_down = false;
		}
		else if(e->key() == Qt::Key::Key_Left)
		{
			left_down = false;
		}
		else if(e->key() == Qt::Key::Key_Right)
		{
			right_down = false;
		}
		else if(e->key() == Qt::Key::Key_Up)
		{
			up_down = false;
		}
		else if(e->key() == Qt::Key::Key_Down)
		{
			down_down = false;
		}
		else if(e->key() == Qt::Key::Key_B)
		{
			B_down = false;
		}
	}
}


// If this widget loses focus, just consider all keys up.
// Otherwise we might miss the key-up event, leading to our keys appearing to be stuck down.
void GlWidget::focusOutEvent(QFocusEvent* e)
{
	// conPrint("GlWidget::focusOutEvent");

	SHIFT_down = false;
	CTRL_down = false;
	A_down = false;
	W_down = false;
	S_down = false;
	D_down = false;
	space_down = false;
	C_down = false; 
	left_down = false;
	right_down = false;
	up_down = false;
	down_down = false;
	B_down = false;
}


void GlWidget::processPlayerPhysicsInput(float dt, PlayerPhysicsInput& input_out)
{
	if(!player_physics)
		return;

	bool cam_changed = false;
	bool move_key_pressed = false;

	// Handle gamepad input
	double gamepad_strafe_leftright = 0;
	double gamepad_strafe_forwardsback = 0;
	double gamepad_turn_leftright = 0;
	double gamepad_turn_updown = 0;
#if 0
	if(gamepad)
	{
		gamepad_strafe_leftright = gamepad->axisLeftX();
		gamepad_strafe_forwardsback = gamepad->axisLeftY();

		gamepad_turn_leftright = gamepad->axisRightX();
		gamepad_turn_updown = gamepad->axisRightY();

		//if(gamepad->axisLeftY() != 0)
		//{ this->cam_controller->update(/*pos delta=*/Vec3d(0.0), Vec2d(0, dt *  gamepad->axisLeftY())); cam_changed = true; }

		//if(gamepad->axisRightX() != 0)
		//{ this->cam_controller->update(/*pos delta=*/Vec3d(0.0), Vec2d(0, dt * -gamepad->axisRightX())); cam_changed = true; }

		//if(gamepad->axisRightY() != 0)
		//{ this->cam_controller->update(/*pos delta=*/Vec3d(0.0), Vec2d(0, dt *  gamepad->axisRightY())); cam_changed = true; }
	}
#endif

	if(gamepad_strafe_leftright != 0 || gamepad_strafe_forwardsback != 0 || gamepad_turn_leftright != 0 || gamepad_turn_updown != 0)
		cam_changed = true;

	input_out.SHIFT_down =	false;
	input_out.CTRL_down =	false;
	input_out.W_down =		false;
	input_out.S_down =		false;
	input_out.A_down =		false;
	input_out.D_down =		false;
	input_out.space_down =	false;
	input_out.C_down =		false;
	input_out.left_down =	false;
	input_out.right_down =	false;
	input_out.up_down =		false;
	input_out.down_down =	false;
	input_out.B_down =		false;


	// On Windows we will use GetAsyncKeyState() to test if a key is down.
	// On Mac OS / Linux we will use our W_down etc.. state.
	// This isn't as good because if we miss the keyReleaseEvent due to not having focus when the key is released, the key will act as if it's stuck down.
	// TODO: Find an equivalent solution to GetAsyncKeyState on Mac/Linux.
	if(hasFocus() && cam_move_on_key_input_enabled)
	{
#ifdef _WIN32
		SHIFT_down =	GetAsyncKeyState(VK_SHIFT);
		CTRL_down	=	GetAsyncKeyState(VK_CONTROL);
		W_down =		GetAsyncKeyState('W');
		S_down =		GetAsyncKeyState('S');
		A_down =		GetAsyncKeyState('A');
		D_down =		GetAsyncKeyState('D');
		space_down =	GetAsyncKeyState(' ');
		C_down =		GetAsyncKeyState('C');
		left_down =		GetAsyncKeyState(VK_LEFT);
		right_down =	GetAsyncKeyState(VK_RIGHT);
		up_down =		GetAsyncKeyState(VK_UP);
		down_down =		GetAsyncKeyState(VK_DOWN);
		B_down = 		GetAsyncKeyState('B');
#else
		CTRL_down = QApplication::keyboardModifiers().testFlag(Qt::ControlModifier);
#endif
		const float selfie_move_factor = cam_controller->selfieModeEnabled() ? -1.f : 1.f;

		if(W_down || up_down)
		{	this->player_physics->processMoveForwards(1.f * selfie_move_factor, SHIFT_down, *this->cam_controller); cam_changed = true; move_key_pressed = true; }
		if(S_down || down_down)
		{	this->player_physics->processMoveForwards(-1.f * selfie_move_factor, SHIFT_down, *this->cam_controller); cam_changed = true; move_key_pressed = true; }
		if(A_down)
		{	this->player_physics->processStrafeRight(-1.f * selfie_move_factor, SHIFT_down, *this->cam_controller); cam_changed = true; move_key_pressed = true; }
		if(D_down)
		{	this->player_physics->processStrafeRight(1.f * selfie_move_factor, SHIFT_down, *this->cam_controller); cam_changed = true; move_key_pressed = true; }

		// Move vertically up or down in flymode.
		if(space_down)
		{	this->player_physics->processMoveUp(1.f, SHIFT_down, *this->cam_controller); cam_changed = true; move_key_pressed = true; }
		if(C_down && !CTRL_down) // Check CTRL_down to prevent CTRL+C shortcut moving camera up.
		{	this->player_physics->processMoveUp(-1.f, SHIFT_down, *this->cam_controller); cam_changed = true; move_key_pressed = true; }

		// Turn left or right
		const float base_rotate_speed = 200;
		if(left_down)
		{	this->cam_controller->update(/*pos delta=*/Vec3d(0.0), Vec2d(0, dt * -base_rotate_speed * (SHIFT_down ? 3.0 : 1.0))); cam_changed = true; }
		if(right_down)
		{	this->cam_controller->update(/*pos delta=*/Vec3d(0.0), Vec2d(0, dt *  base_rotate_speed * (SHIFT_down ? 3.0 : 1.0))); cam_changed = true; }

		if(cam_changed)
			emit cameraUpdated();

		input_out.SHIFT_down =	SHIFT_down;
		input_out.CTRL_down =	CTRL_down;
		input_out.W_down =		W_down;
		input_out.S_down =		S_down;
		input_out.A_down =		A_down;
		input_out.D_down =		D_down;
		input_out.space_down =	space_down;
		input_out.C_down =		C_down;
		input_out.left_down =	left_down;
		input_out.right_down =	right_down;
		input_out.up_down =		up_down;
		input_out.down_down =	down_down;
		input_out.B_down	=	B_down;
	}

#if 0
	if(gamepad)
	{
		const float gamepad_move_speed_factor = 10;
		const float gamepad_rotate_speed = 400;

		// Move vertically up or down in flymode.
		if(gamepad->buttonR2() != 0)
		{	this->player_physics->processMoveUp(gamepad_move_speed_factor * pow(gamepad->buttonR2(), 3.f), SHIFT_down, *this->cam_controller); cam_changed = true; move_key_pressed = true; }

		if(gamepad->buttonL2() != 0)
		{	this->player_physics->processMoveUp(gamepad_move_speed_factor * -pow(gamepad->buttonL2(), 3.f), SHIFT_down, *this->cam_controller); cam_changed = true; move_key_pressed = true; }

		if(gamepad->axisLeftX() != 0)
		{	this->player_physics->processStrafeRight(gamepad_move_speed_factor * pow(gamepad->axisLeftX(), 3.f), SHIFT_down, *this->cam_controller); cam_changed = true; move_key_pressed = true; }

		if(gamepad->axisLeftY() != 0)
		{	this->player_physics->processMoveForwards(gamepad_move_speed_factor * -pow(gamepad->axisLeftY(), 3.f), SHIFT_down, *this->cam_controller); cam_changed = true; move_key_pressed = true; }

		if(gamepad->axisRightX() != 0)
		{ this->cam_controller->update(/*pos delta=*/Vec3d(0.0), Vec2d(0, dt * gamepad_rotate_speed * pow(gamepad->axisRightX(), 3.0f))); cam_changed = true; }

		if(gamepad->axisRightY() != 0)
		{ this->cam_controller->update(/*pos delta=*/Vec3d(0.0), Vec2d(dt *  gamepad_rotate_speed * -pow(gamepad->axisRightY(), 3.f), 0)); cam_changed = true; }
	}
#endif


	if(cam_changed)
		emit cameraUpdated();

	if(move_key_pressed)
		emit playerMoveKeyPressed();
}


void GlWidget::hideCursor()
{
	// Hide cursor when moving view.
	this->setCursor(QCursor(Qt::BlankCursor));
}


bool GlWidget::isCursorHidden()
{
	return this->cursor().shape() == Qt::BlankCursor;
}


void GlWidget::setCursorIfNotHidden(Qt::CursorShape new_shape)
{
	if(this->cursor().shape() != Qt::BlankCursor)
		this->setCursor(new_shape);
}


void GlWidget::mousePressEvent(QMouseEvent* e)
{
	//conPrint("mousePressEvent at " + toString(QCursor::pos().x()) + ", " + toString(QCursor::pos().y()));
	mouse_move_origin = QCursor::pos();
	last_mouse_press_pos = QCursor::pos();

	// Hide cursor when moving view.
	//this->setCursor(QCursor(Qt::BlankCursor));

	emit mousePressed(e);
}


void GlWidget::mouseReleaseEvent(QMouseEvent* e)
{
	// Unhide cursor.
	this->unsetCursor();

	//conPrint("mouseReleaseEvent at " + toString(QCursor::pos().x()) + ", " + toString(QCursor::pos().y()));

	//if((QCursor::pos() - last_mouse_press_pos).manhattanLength() < 4)
	{
		//conPrint("Click at " + toString(QCursor::pos().x()) + ", " + toString(QCursor::pos().y()));
		//conPrint("Click at " + toString(e->pos().x()) + ", " + toString(e->pos().y()));

		emit mouseClicked(e);
	}
}


void GlWidget::showEvent(QShowEvent* e)
{
	emit widgetShowSignal();
}


void GlWidget::mouseMoveEvent(QMouseEvent* e)
{
	Qt::MouseButtons mb = e->buttons();
	if(cam_rot_on_mouse_move_enabled && (cam_controller != NULL) && (mb & Qt::LeftButton))// && (e->modifiers() & Qt::AltModifier))
	{
		/*switch(e->source())
		{
			case Qt::MouseEventNotSynthesized: conPrint("Qt::MouseEventNotSynthesized");
			case Qt::MouseEventSynthesizedBySystem: conPrint("Qt::MouseEventSynthesizedBySystem");
			case Qt::MouseEventSynthesizedByQt: conPrint("Qt::MouseEventSynthesizedByQt");
			case Qt::MouseEventSynthesizedByApplication: conPrint("Qt::MouseEventSynthesizedByApplication");
		}*/

		//double shift_scale = ((e->modifiers() & Qt::ShiftModifier) == 0) ? 1.0 : 0.35; // If shift is held, movement speed is roughly 1/3

		// Get new mouse position, movement vector and set previous mouse position to new.
		QPoint new_pos = QCursor::pos();
		QPoint delta(new_pos.x() - mouse_move_origin.x(),
					 mouse_move_origin.y() - new_pos.y()); // Y+ is down in screenspace, not up as desired.

		// QCursor::setPos() seems to generate mouse move events, which we don't want to affect the camera.  Only rotate camera based on actual mouse movements.
		if(e->source() == Qt::MouseEventNotSynthesized)
		{
			if(mb & Qt::LeftButton)  cam_controller->update(Vec3d(0, 0, 0), Vec2d(delta.y(), delta.x())/* * shift_scale*/);
			//if(mb & Qt::MidButton)   cam_controller->update(Vec3d(delta.x(), 0, delta.y()) * shift_scale, Vec2d(0, 0));
			//if(mb & Qt::RightButton) cam_controller->update(Vec3d(0, delta.y(), 0) * shift_scale, Vec2d(0, 0));

			if(mb & Qt::RightButton || mb & Qt::LeftButton || mb & Qt::MiddleButton)
				emit cameraUpdated();
		}

		// On Windows/linux, reset the cursor position to where we started, so we never run out of space to move.
		// QCursor::setPos() does not work on mac, and also gives a message about Substrata trying to control the computer, which we want to avoid.
		// So don't use setPos() on Mac.
#if defined(OSX)
		mouse_move_origin = QCursor::pos();
#else
		QCursor::setPos(mouse_move_origin);
		mouse_move_origin = QCursor::pos();
#endif

		//conPrint("mouseMoveEvent FPS: " + doubleToStringNSigFigs(1 / fps_timer.elapsed(), 1));
		//fps_timer.reset();
	}

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
	QOpenGLWidget::mouseMoveEvent(e);
#else
	QGLWidget::mouseMoveEvent(e);
#endif

	emit mouseMoved(e);

	//conPrint("mouseMoveEvent time since last event: " + doubleToStringNSigFigs(fps_timer.elapsed(), 5));
	//fps_timer.reset();
}


void GlWidget::wheelEvent(QWheelEvent* e)
{
	emit mouseWheelSignal(e);
}


void GlWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
	emit mouseDoubleClickedSignal(e);
}
