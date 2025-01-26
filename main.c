#include <math.h>
#include <SDL.h>
#include <GL/glew.h>
#include <SDL_opengl.h>
#include <stdio.h>
#include <string.h>
#include "bg4.h"

//#define USING_FRAMEBUFFER 1

// Convert define to string, expanding before stringifying.
#define STR_( X ) #X
#define STR( X ) STR_( X )

#define WINDOW_WIDTH_PIXELS 512
#define WINDOW_HEIGHT_PIXELS 288

#define YMAXSPEED 128.0f
#define YACC 16.0f
#define YMIN 256.0f
#define YMAX 512.0f
#define XACC 64.0f
#define XMAXSPEED 128.0f

#define MAXFPSCOUNT 100

static unsigned int running = 1;
static SDL_Window * window;
static SDL_GLContext context;
static unsigned int magnification = 11;
static float vertices[] = {
	-1.0f, -1.0f, -1.0f, // Lower left
	1.0f, -1.0f, -1.0f,  // Lower right
	1.0f, 1.0f, -1.0f,   // Upper right
	-1.0f, 1.0f, -1.0f   // Upper left
};
static int indices[] = {
    0, 1, 3,
    1, 2, 3
};
static struct
{
	int left;
	int right;
	int up;
	int down;
	int z;
	int x;
	int c;
} input;
static GLuint vbo;
static struct
{
	float x;
	float y;
	float z;
} trans;
static GLuint texture;
static GLuint fb_texture;
static GLint yoffset_uniform;
static GLint xoffset_uniform;
static GLint texture_width_uniform;
static GLint texture_height_uniform;
static GLint texture_uniform;
static GLint ftexture_uniform;
static float yoffset = YMIN;
static float yoffsetacc = YACC;
static float yoffsetspeed = 0.0f;
static float xacc = 0.5f;
static float vx = 0.0f;
static float xoffset = 0.0f;
static float fps = 0.0f;
static float fpslist[ MAXFPSCOUNT ];
static unsigned int fpsindex = 0;
static unsigned int framecount = 0;
static unsigned int fbo;
static GLuint program;
static GLuint fprogram;
static unsigned int screen_width;
static unsigned int screen_height;

static void init();
static void loop();
static void update( float dt );
static void render();
static void close();
static void update_screen();
static void update_viewport();
static void init_fps();
static void update_fps( float dt );
static void print_fps();

void main()
{
	init();
	loop();
	close();
}

static void init()
{
	SDL_Init( SDL_INIT_EVERYTHING );
	window = SDL_CreateWindow( "CV4", 100, 100, WINDOW_WIDTH_PIXELS * magnification, WINDOW_HEIGHT_PIXELS * magnification, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE );
	context = SDL_GL_CreateContext( window );
	glewInit();

	// Set up viewport.
	screen_width = WINDOW_WIDTH_PIXELS * magnification;
	screen_height = WINDOW_HEIGHT_PIXELS * magnification;
	update_viewport();

	// Init VBO.
	glGenBuffers( 1, &vbo );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, vbo );
	glBufferData( GL_ARRAY_BUFFER, sizeof( vertices ), vertices, GL_STATIC_DRAW );
	glBufferData( GL_ELEMENT_ARRAY_BUFFER, sizeof( indices ), indices, GL_STATIC_DRAW );

	// Init texture.
	const unsigned int texture_size = texture_width * texture_height * 4;
	unsigned char texture_data[ texture_size ];
	char pixel[ 3 ];
	char * pixel_data = header_data;
	for ( unsigned int i = 0; i < texture_size; i += 4 )
	{
		HEADER_PIXEL( pixel_data, pixel );
		memcpy( &texture_data[ i ], pixel, 3 );
		texture_data[ i + 3 ] = 0xFF;
	}

	// Init shaders.
	const char * vertex_shader =
		"#version 330\n"
		"layout(location = 0) in vec3 position;\n"
		"out vec2 coords;\n"
		"out vec2 texture_coords;\n"
		"const float screen_width = " STR( WINDOW_WIDTH_PIXELS ) ";\n"
		"const float screen_height = " STR( WINDOW_HEIGHT_PIXELS ) ";\n"
		"uniform float yoffset;\n"
		"uniform float texture_width;\n"
		"uniform float texture_height;\n"
		"\n"
		"float ypixels( float y )\n"
		"{\n"
		"	return y / texture_height;\n"
		"}\n"
		"\n"
		"float xpixels( float x )\n"
		"{\n"
		"	return x / texture_width;\n"
		"}\n"
		"\n"
		"void main()\n"
		"{\n"
		"	gl_Position = vec4( position, 1.0 );\n"
		"	coords = vec2(\n"
		"		// Normalize x coords to be 0 – 1, going left to right.\n"
		"		( position.x / 2.0 + 0.5 ),\n"
		"		// Normalize y coords to be 0 – 1, going top to bottom.\n"
		"		( position.y / -2.0 + 0.5 )\n"
		"	);\n"
		"	texture_coords = vec2(\n"
		"		// Note: not used.\n"
		"		coords.x * xpixels( screen_width ),\n"
		"		// Only render portion o’ texture that fits in screen to prevent stretching\n"
		"		// & offset by yoffset pixels.\n"
		"		coords.y * ypixels( screen_height ) + ypixels( yoffset )\n"
		"	);\n"
		"}\n";
	const char * fragment_shader =
		"#version 330\n"
		"in vec2 texture_coords;\n"
		"in vec2 coords;\n"
		"const float screen_width = " STR( WINDOW_WIDTH_PIXELS ) ";\n"
		"uniform sampler2D frag_texture;\n"
		"uniform float texture_width;\n"
		"uniform float xoffset;\n"
		"\n"
		"float xpixels( float x )\n"
		"{\n"
		"	return x / texture_width;\n"
		"}\n"
		"\n"
		"void main()\n"
		"{\n"
		"	// 4x^2 - 4x + 1 -> f(0) = 1, f(0.5) = 0, f(1) = 1.\n"
		"	float xtransform = 4 * pow( coords.y, 2.0 ) - 4 * coords.y + 1;\n"
		"	// Zoom in to 25% – 75% o’ texture @ edges.\n"
		"	float frag_coords_x = ( coords.x * ( 1 - ( 0.5 * xtransform ) ) + 0.25 * xtransform ) * xpixels( screen_width ) + xpixels( xoffset );\n"
		"	gl_FragColor = vec4(\n"
		"		// Darken center.\n"
		"		texture( frag_texture, vec2( frag_coords_x, texture_coords.y ) ).rgb - 0.3 + 0.2 * xtransform,\n"
		"		1.0\n"
		"	);\n"
		"}\n";
	const char * f_vertex_shader =
		"#version 330\n"
		"layout(location = 0) in vec3 position;\n"
		"out vec2 coords;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	gl_Position = vec4( position, 1.0 );\n"
		"	coords = vec2(\n"
		"		// Normalize x coords to be 0 – 1, going left to right.\n"
		"		( position.x / 2.0 + 0.5 ),\n"
		"		// Normalize y coords to be 0 – 1, going top to bottom.\n"
		"		( position.y / -2.0 + 0.5 )\n"
		"	);\n"
		"}\n";
	const char * f_fragment_shader =
		"#version 330\n"
		"in vec2 coords;\n"
		"uniform sampler2D frag_texture;\n"
		"\n"
		"void main()\n"
		"{\n"
		"	gl_FragColor = texture( frag_texture, coords );\n"
		"}\n";
	GLuint vs = glCreateShader( GL_VERTEX_SHADER );
	GLuint fs = glCreateShader( GL_FRAGMENT_SHADER );
	glShaderSource( vs, 1, &vertex_shader, NULL );
	glShaderSource( fs, 1, &fragment_shader, NULL );
	glCompileShader( vs );
	glCompileShader( fs );
	program = glCreateProgram();
	glAttachShader( program, vs );
	glAttachShader( program, fs );
	glLinkProgram( program );
    // Test shader program linking was successful.
    int success;
    char log[ 512 ];
    glGetProgramiv( program, GL_LINK_STATUS, &success );
    if ( !success )
    {
        glGetProgramInfoLog( program, 512, NULL, log );
        printf( "Shader program linking failed! %s\n", log );
    }
	glUseProgram( program );

	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, texture_width, texture_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, texture_data );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	texture_width_uniform = glGetUniformLocation( program, "texture_width" );
	glUniform1f( texture_width_uniform, ( float )( texture_width ) );
	texture_height_uniform = glGetUniformLocation( program, "texture_height" );
	glUniform1f( texture_height_uniform, ( float )( texture_height ) );
	yoffset_uniform = glGetUniformLocation( program, "yoffset" );
	xoffset_uniform = glGetUniformLocation( program, "xoffset" );
	texture_uniform = glGetUniformLocation( program, "frag_texture" );
	glUniform1i( texture_uniform, 0 );

	#if USING_FRAMEBUFFER
		// Init framebuffer shader.
		GLuint fvs = glCreateShader( GL_VERTEX_SHADER );
		GLuint ffs = glCreateShader( GL_FRAGMENT_SHADER );
		glShaderSource( fvs, 1, &f_vertex_shader, NULL );
		glShaderSource( ffs, 1, &f_fragment_shader, NULL );
		glCompileShader( fvs );
		glCompileShader( ffs );
		fprogram = glCreateProgram();
		glAttachShader( fprogram, fvs );
		glAttachShader( fprogram, ffs );
		glLinkProgram( fprogram );
		// Test shader program linking was successful.
		glGetProgramiv( fprogram, GL_LINK_STATUS, &success );
		if ( !success )
		{
			glGetProgramInfoLog( fprogram, 512, NULL, log );
			printf( "Shader fprogram linking failed! %s\n", log );
		}
		glUseProgram( fprogram );
		ftexture_uniform = glGetUniformLocation( program, "frag_texture" );
		glUniform1i( ftexture_uniform, 0 );

		// Generate framebuffer.
		glGenFramebuffers( 1, &fbo );
		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glGenTextures( 1, &fb_texture );
		glBindTexture( GL_TEXTURE_2D, fb_texture );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, WINDOW_WIDTH_PIXELS, WINDOW_HEIGHT_PIXELS, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb_texture, 0 );
		if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		{
			printf( "Framebuffer not complete!\n" );
		}
		glBindFramebuffer( GL_FRAMEBUFFER, 0 );
		glUseProgram( 0 );
	#endif

	// Don't draw back faces.
	glCullFace( GL_BACK );

	// Make sure vsync is off.
	SDL_GL_SetSwapInterval( 0 );

	init_fps();
}

static void loop()
{
	Uint32 prev_ticks = SDL_GetTicks();
	while( running )
	{
		SDL_Event event;
		while ( SDL_PollEvent( &event ) )
		{
			switch ( event.type )
			{
				case ( SDL_QUIT ):
					running = 0;
				break;
				case ( SDL_KEYDOWN ):
					switch ( event.key.keysym.sym ) {
						case ( SDLK_ESCAPE ):
							running = 0;
						break;
						case ( SDLK_LEFT ):
							input.left = 1;
						break;
						case ( SDLK_RIGHT ):
							input.right = 1;
						break;
					}
				break;
				case ( SDL_KEYUP ):
					switch ( event.key.keysym.sym ) {
						case ( SDLK_LEFT ):
							input.left = 0;
						break;
						case ( SDLK_RIGHT ):
							input.right = 0;
						break;
					}
				break;
				case SDL_WINDOWEVENT:
					// On window resize.
					if ( event.window.event == SDL_WINDOWEVENT_RESIZED )
					{
						screen_width = event.window.data1;
						screen_height = event.window.data2;
						update_screen();
					}
				break;
			}
		}

		Uint32 ticks = SDL_GetTicks();
		float dt = ( ticks - prev_ticks ) / 1000.0f;
		update( dt );
		prev_ticks = ticks;

		render();

		update_fps( dt );
	}
}

static void update( float dt )
{
	if ( input.left )
	{
		xacc = -XACC;
	}
	else if ( input.right )
	{
		xacc = XACC;
	}
	else
	{
		xacc = 0.0f;
		vx /= 1.0f + 0.05f * dt;
	}

	vx += xacc * dt;
	if ( vx > XMAXSPEED )
	{
		vx = XMAXSPEED;
	}
	else if ( vx < -XMAXSPEED )
	{
		vx = -XMAXSPEED;
	}
	xoffset += vx * dt;

	yoffsetspeed += yoffsetacc * dt;
	if ( yoffsetspeed > YMAXSPEED )
	{
		yoffsetspeed = YMAXSPEED;
	}
	else if ( yoffsetspeed < -YMAXSPEED )
	{
		yoffsetspeed = -YMAXSPEED;
	}
	yoffset += yoffsetspeed * dt;
	if ( yoffset > YMAX )
	{
		yoffsetacc = -YACC;
	}
	else if ( yoffset < YMIN )
	{
		yoffsetacc = YACC;
	}
}

static void render()
{
	glClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	#if USING_FRAMEBUFFER
		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glViewport( 0, 0, WINDOW_WIDTH_PIXELS, WINDOW_HEIGHT_PIXELS );
	#endif

	glUseProgram( program );
	glBindTexture( GL_TEXTURE_2D, texture );
	glUniform1f( texture_width_uniform, ( float )( texture_width ) );
	glUniform1f( texture_height_uniform, ( float )( texture_height ) );
	glUniform1i( texture_uniform, 0 );
	glUniform1f( yoffset_uniform, yoffset );
	glUniform1f( xoffset_uniform, xoffset );
	glEnableClientState( GL_VERTEX_ARRAY );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, vbo );
	glVertexPointer( 3, GL_FLOAT, 0, &vertices );
	glDrawElements( GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0 );
	glDisableClientState( GL_VERTEX_ARRAY );

	#if USING_FRAMEBUFFER
		glBindFramebuffer( GL_FRAMEBUFFER, 0 );
		update_viewport();
		glUseProgram( fprogram );
		glBindTexture( GL_TEXTURE_2D, fb_texture );
		glEnableClientState( GL_VERTEX_ARRAY );
		glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, vbo );
		glVertexPointer( 3, GL_FLOAT, 0, &vertices );
		glDrawElements( GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0 );
		glDisableClientState( GL_VERTEX_ARRAY );
	#endif
	SDL_GL_SwapWindow( window );
}

static void close()
{
	print_fps();
	glDeleteBuffers( 1, &vbo );
	SDL_GL_DeleteContext( context );
	SDL_DestroyWindow( window );
	SDL_Quit();
}

static void update_screen()
{
	const double screen_aspect_ratio = ( double )( WINDOW_WIDTH_PIXELS ) / ( double )( WINDOW_HEIGHT_PIXELS );
	const double monitor_aspect_ratio = ( double )( screen_width ) / ( double )( screen_height );

	// Base magnification on max that fits in window.
	magnification = 
		( unsigned int )( floor(
			( monitor_aspect_ratio > screen_aspect_ratio )
				? ( double )( screen_height ) / ( double )( WINDOW_HEIGHT_PIXELS )
				: ( double )( screen_width ) / ( double )( WINDOW_WIDTH_PIXELS )
		));

	// Clamp minimum magnification to 1.
	if ( magnification < 1 )
	{
		magnification = 1;
	}

	update_viewport();
}

static void update_viewport()
{
	float viewportw = WINDOW_WIDTH_PIXELS * magnification;
	float viewporth = WINDOW_HEIGHT_PIXELS * magnification;
	float viewportx = floor( ( double )( screen_width - viewportw ) / 2.0 );
	float viewporty = floor( ( double )( screen_height - viewporth ) / 2.0 );
	glViewport( viewportx, viewporty, viewportw, viewporth );
}

static void init_fps()
{
	memset( fpslist, 0, sizeof( fpslist ) );
}

static void update_fps( float dt )
{
	if ( fpsindex >= MAXFPSCOUNT )
	{
		//running = 0;
		return;
	}
	fps += dt;
	++framecount;
	if ( fps > 1.0f )
	{
		fpslist[ fpsindex++ ] = ( float )( framecount );
		framecount = 0;
		fps = 0.0f;
	}
}

static void print_fps()
{
	float minfps = 999999.9f;
	float maxfps = 0.0f;
	float totalfps = 0.0f;
	for ( unsigned int i = 0; i < fpsindex; ++i )
	{
		if ( fpslist[ i ] < minfps )
		{
			minfps = fpslist[ i ];
		}
		if ( fpslist[ i ] > maxfps )
		{
			maxfps = fpslist[ i ];
		}
		totalfps += fpslist[ i ];
	}
	float avgfps = totalfps / ( float )( fpsindex );

	printf( "Min FPS: %f\n", minfps );
	printf( "Avg FPS: %f\n", avgfps );
	printf( "Max FPS: %f\n", maxfps );
}
