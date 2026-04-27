#ifdef PLATFORM_SDL2
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>
#endif

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#include "global.h"
#include "platform.h"
#include "rtc.h"
#include "gba/defines.h"
#include "gba/m4a_internal.h"
#include "cgb_audio.h"
#include "gba/flash_internal.h"
#include "platform/dma.h"
#include "platform/framedraw.h"

extern void (*const gIntrTable[])(void);

SDL_Thread *mainLoopThread;
SDL_Window *sdlWindow;
SDL_Renderer *sdlRenderer;
SDL_Texture *sdlTexture;
SDL_GLContext sdlGlContext;
SDL_sem *vBlankSemaphore;
SDL_atomic_t isFrameAvailable;
bool speedUp = false;
unsigned int videoScale = 1;
bool isRunning = true;
bool paused = false;
bool useShaderPipeline = false;
bool audioOutputReady = false;
float audioVolume = 1.0f;
int audioSourceSampleRate = 42048;
int audioRequestedSampleRate = 42048;
int audioDeviceSampleRate = 42048;
SDL_AudioDeviceID audioDeviceId = 0;
SDL_AudioStream *audioStream = NULL;
float *audioVolumeBuffer = NULL;
size_t audioVolumeBufferFloats = 0;
Uint8 *audioOutputBuffer = NULL;
size_t audioOutputBufferBytes = 0;
double simTime = 0;
double lastGameTime = 0;
double curGameTime = 0;
double fixedTimestep = 1.0 / 60.0; // 16.666667ms
double timeScale = 1.0;
GLuint glProgram = 0;
GLuint glFrameTexture = 0;
GLuint glPrevFrameTexture = 0;
GLuint glTopBgTexture = 0;
GLuint glTopSpriteTexture = 0;
GLuint glQuadVao = 0;
GLuint glQuadVbo = 0;
GLint glMainTextureUniform = -1;
GLint glPrevFrameTextureUniform = -1;
GLint glTopBgTextureUniform = -1;
GLint glTopSpriteTextureUniform = -1;
GLint glRenderResolutionUniform = -1;
GLint glSourceResolutionUniform = -1;
GLint glOutputResolutionUniform = -1;
GLint glTimeUniform = -1;
GLint glFrameUniform = -1;
GLint glScaleUniform = -1;
GLint glAudioVolumeUniform = -1;
GLint glSpeedUniform = -1;
Uint32 shaderStartTicks = 0;
int shaderFrameCounter = 0;
char fragmentShaderPath[260] = "shaders/upscale.frag";
struct SiiRtcInfo internalClock;
static bool showVolumeNotice = false;
static Uint32 volumeNoticeUntil = 0;
static char volumeNoticeText[32] = {0};

struct PcConfig
{
    bool enableShader;
    unsigned int windowScale;
    char shaderPath[260];
    bool enableAudio;
    int audioSampleRate;
    unsigned int audioBufferSamples;
    float audioVolume;
};

static PFNGLCREATESHADERPROC pglCreateShader;
static PFNGLSHADERSOURCEPROC pglShaderSource;
static PFNGLCOMPILESHADERPROC pglCompileShader;
static PFNGLGETSHADERIVPROC pglGetShaderiv;
static PFNGLGETSHADERINFOLOGPROC pglGetShaderInfoLog;
static PFNGLDELETESHADERPROC pglDeleteShader;
static PFNGLCREATEPROGRAMPROC pglCreateProgram;
static PFNGLATTACHSHADERPROC pglAttachShader;
static PFNGLLINKPROGRAMPROC pglLinkProgram;
static PFNGLGETPROGRAMIVPROC pglGetProgramiv;
static PFNGLGETPROGRAMINFOLOGPROC pglGetProgramInfoLog;
static PFNGLUSEPROGRAMPROC pglUseProgram;
static PFNGLDELETEPROGRAMPROC pglDeleteProgram;
static PFNGLGETUNIFORMLOCATIONPROC pglGetUniformLocation;
static PFNGLUNIFORM1IPROC pglUniform1i;
static PFNGLUNIFORM1FPROC pglUniform1f;
static PFNGLUNIFORM2FPROC pglUniform2f;
static PFNGLACTIVETEXTUREPROC pglActiveTexture;
static PFNGLGENVERTEXARRAYSPROC pglGenVertexArrays;
static PFNGLBINDVERTEXARRAYPROC pglBindVertexArray;
static PFNGLDELETEVERTEXARRAYSPROC pglDeleteVertexArrays;
static PFNGLGENBUFFERSPROC pglGenBuffers;
static PFNGLBINDBUFFERPROC pglBindBuffer;
static PFNGLBUFFERDATAPROC pglBufferData;
static PFNGLDELETEBUFFERSPROC pglDeleteBuffers;
static PFNGLVERTEXATTRIBPOINTERPROC pglVertexAttribPointer;
static PFNGLENABLEVERTEXATTRIBARRAYPROC pglEnableVertexAttribArray;
typedef void (APIENTRYP PokeGlDrawArraysProc)(GLenum mode, GLint first, GLsizei count);
static PokeGlDrawArraysProc pglDrawArrays;

static FILE *sSaveFile = NULL;

extern void AgbMain(void);
extern void DoSoftReset(void);

int DoMain(void *param);
void ProcessEvents(void);
void VDraw(SDL_Texture *texture);

static void ReadSaveFile(char *path);
static void StoreSaveFile(void);
static void CloseSaveFile(void);

static void UpdateInternalClock(void);
static bool TryInitShaderPipeline(void);
static void ShutdownShaderPipeline(void);
static void RenderFrameWithShader(void);
static bool ShaderFileExists(const char *path);
static bool LoadShaderApi(void);
static GLuint CompileShader(GLenum type, const char *source, const char *label);
static char *LoadTextFile(const char *path);
static void LoadPcConfig(const char *path, struct PcConfig *config);
static char *TrimWhitespace(char *text);
static bool ParseBoolValue(const char *value, bool defaultValue);
static void ShowVolumeNotice(float volume);
static void AdjustAudioVolume(float delta);
static void DrawVolumeOverlay(uint16_t *pixels);
static void DrawOverlayBox(uint16_t *pixels, int x, int y, int w, int h, uint16_t color);
static const uint8_t *GetOverlayGlyph(char ch);
static void DrawOverlayChar(uint16_t *pixels, int x, int y, char ch, uint16_t color);
static int OverlayTextWidth(const char *text);
static void DrawOverlayText(uint16_t *pixels, int x, int y, const char *text, uint16_t color);

#define RGB555(r, g, b) ((uint16_t)((r) | ((g) << 5) | ((b) << 10)))
#define RGB555_WHITE RGB555(31, 31, 31)

int main(int argc, char **argv)
{
    Uint32 windowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    const char *shaderEnv;
    const char *windowScaleEnv;
    bool shaderRequested;
    bool shaderFilePresent;
    struct PcConfig config;

    // Open an output console on Windows
#ifdef _WIN32
    AllocConsole() ;
    AttachConsole( GetCurrentProcessId() ) ;
    freopen( "CON", "w", stdout ) ;
#endif

    config.enableShader = false;
    config.windowScale = 1;
    strncpy(config.shaderPath, fragmentShaderPath, sizeof(config.shaderPath) - 1);
    config.shaderPath[sizeof(config.shaderPath) - 1] = '\0';
    config.enableAudio = true;
    config.audioSampleRate = 42048;
    config.audioBufferSamples = 1024;
    config.audioVolume = 1.0f;
    LoadPcConfig("pc_config.ini", &config);
    audioSourceSampleRate = 42048;
    audioRequestedSampleRate = config.audioSampleRate;
    audioVolume = config.audioVolume;

    videoScale = config.windowScale;
    strncpy(fragmentShaderPath, config.shaderPath, sizeof(fragmentShaderPath) - 1);
    fragmentShaderPath[sizeof(fragmentShaderPath) - 1] = '\0';

    ReadSaveFile("pokeemerald.sav");

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0)
    {
        DBGPRINTF("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

    shaderEnv = SDL_getenv("POKEEMERALD_ENABLE_SHADER");
    shaderRequested = config.enableShader;
    if (shaderEnv != NULL)
        shaderRequested = ParseBoolValue(shaderEnv, shaderRequested);

    windowScaleEnv = SDL_getenv("POKEEMERALD_WINDOW_SCALE");
    if (windowScaleEnv != NULL)
    {
        unsigned int envScale = (unsigned int)strtoul(windowScaleEnv, NULL, 10);
        if (envScale >= 1 && envScale <= 10)
            videoScale = envScale;
    }

    shaderFilePresent = ShaderFileExists(fragmentShaderPath);
    if (shaderRequested && shaderFilePresent)
        windowFlags |= SDL_WINDOW_OPENGL;

    sdlWindow = SDL_CreateWindow("pokeemerald", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, DISPLAY_WIDTH * videoScale, DISPLAY_HEIGHT * videoScale, windowFlags);
    if (sdlWindow == NULL)
    {
        DBGPRINTF("Window could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

    if (shaderRequested && shaderFilePresent && TryInitShaderPipeline())
    {
        useShaderPipeline = true;
        DBGPRINTF("Using OpenGL shader pipeline (%s)\n", fragmentShaderPath);
    }
    else
    {
        if (shaderRequested && !shaderFilePresent)
            DBGPRINTF("Shader requested but file '%s' was not found. Using nearest fallback.\n", fragmentShaderPath);

        sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, SDL_RENDERER_PRESENTVSYNC);
        if (sdlRenderer == NULL)
        {
            DBGPRINTF("Renderer could not be created! SDL_Error: %s\n", SDL_GetError());
            return 1;
        }

        SDL_SetRenderDrawColor(sdlRenderer, 255, 255, 255, 255);
        SDL_RenderClear(sdlRenderer);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        SDL_RenderSetLogicalSize(sdlRenderer, DISPLAY_WIDTH, DISPLAY_HEIGHT);

        sdlTexture = SDL_CreateTexture(sdlRenderer,
                                       SDL_PIXELFORMAT_ABGR1555,
                                       SDL_TEXTUREACCESS_STREAMING,
                                       DISPLAY_WIDTH, DISPLAY_HEIGHT);
        if (sdlTexture == NULL)
        {
            DBGPRINTF("Texture could not be created! SDL_Error: %s\n", SDL_GetError());
            return 1;
        }

        DBGPRINTF("Using SDL nearest fallback pipeline\n");
    }

    simTime = curGameTime = lastGameTime = SDL_GetPerformanceCounter();

    isFrameAvailable.value = 0;
    vBlankSemaphore = SDL_CreateSemaphore(0);

    SDL_AudioSpec want;
    SDL_AudioSpec have;

    SDL_memset(&want, 0, sizeof(want)); /* or SDL_zero(want) */
    want.freq = audioRequestedSampleRate;
    want.format = AUDIO_F32;
    want.channels = 2;
    want.samples = config.audioBufferSamples;
    cgb_audio_init(audioSourceSampleRate);

    if (config.enableAudio)
    {
        SDL_SetHint(SDL_HINT_AUDIO_RESAMPLING_MODE, "best");
        audioDeviceId = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (audioDeviceId == 0)
        {
            SDL_Log("Failed to open audio: %s", SDL_GetError());
            audioOutputReady = false;
        }
        else
        {
            audioDeviceSampleRate = have.freq;
            if (audioDeviceSampleRate != audioRequestedSampleRate)
            {
                SDL_Log("Requested audio rate %d Hz, got %d Hz", audioRequestedSampleRate, audioDeviceSampleRate);
            }
            if (audioDeviceSampleRate != audioSourceSampleRate)
            {
                SDL_Log("Audio resampling enabled: %d Hz -> %d Hz", audioSourceSampleRate, audioDeviceSampleRate);
            }
            audioStream = SDL_NewAudioStream(AUDIO_F32, 2, audioSourceSampleRate, have.format, have.channels, have.freq);
            if (audioStream == NULL)
            {
                SDL_Log("Failed to create audio stream: %s", SDL_GetError());
                SDL_CloseAudioDevice(audioDeviceId);
                audioDeviceId = 0;
                audioOutputReady = false;
            }
            else
            {
                SDL_PauseAudioDevice(audioDeviceId, 0);
                audioOutputReady = true;
            }
        }
    }
    else
    {
        SDL_Log("Audio disabled by pc_config.ini");
        audioOutputReady = false;
    }
    
    VDraw(sdlTexture);
    mainLoopThread = SDL_CreateThread(DoMain, "AgbMain", NULL);

    double accumulator = 0.0;

    memset(&internalClock, 0, sizeof(internalClock));
    internalClock.status = SIIRTCINFO_24HOUR;
    UpdateInternalClock();

    while (isRunning)
    {
        ProcessEvents();

        if (!paused)
        {
            double dt = fixedTimestep / timeScale; // TODO: Fix speedup

            curGameTime = SDL_GetPerformanceCounter();
            double deltaTime = (double)((curGameTime - lastGameTime) / (double)SDL_GetPerformanceFrequency());
            if (deltaTime > (dt * 5))
                deltaTime = dt;
            lastGameTime = curGameTime;

            accumulator += deltaTime;

            while (accumulator >= dt)
            {
                if (SDL_AtomicGet(&isFrameAvailable))
                {
                    VDraw(sdlTexture);
                    if (useShaderPipeline)
                    {
                        RenderFrameWithShader();
                    }
                    else
                    {
                        SDL_RenderClear(sdlRenderer);
                        SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, NULL);
                    }
                    SDL_AtomicSet(&isFrameAvailable, 0);

                    REG_DISPSTAT |= INTR_FLAG_VBLANK;

                    RunDMAs(DMA_HBLANK);

                    if (REG_DISPSTAT & DISPSTAT_VBLANK_INTR)
                        gIntrTable[4]();
                    REG_DISPSTAT &= ~INTR_FLAG_VBLANK;

                    SDL_SemPost(vBlankSemaphore);

                    accumulator -= dt;
                }
            }
        }

        if (useShaderPipeline)
            SDL_GL_SwapWindow(sdlWindow);
        else
            SDL_RenderPresent(sdlRenderer);
    }

    //StoreSaveFile();
    CloseSaveFile();

    if (audioStream != NULL)
    {
        SDL_FreeAudioStream(audioStream);
        audioStream = NULL;
    }
    if (audioDeviceId != 0)
    {
        SDL_CloseAudioDevice(audioDeviceId);
        audioDeviceId = 0;
    }
    free(audioVolumeBuffer);
    audioVolumeBuffer = NULL;
    audioVolumeBufferFloats = 0;
    free(audioOutputBuffer);
    audioOutputBuffer = NULL;
    audioOutputBufferBytes = 0;

    if (sdlTexture != NULL)
        SDL_DestroyTexture(sdlTexture);
    if (sdlRenderer != NULL)
        SDL_DestroyRenderer(sdlRenderer);
    ShutdownShaderPipeline();
    SDL_DestroyWindow(sdlWindow);
    SDL_Quit();
    return 0;
}

static void ReadSaveFile(char *path)
{
    // Check whether the saveFile exists, and create it if not
    sSaveFile = fopen(path, "r+b");
    if (sSaveFile == NULL)
    {
        sSaveFile = fopen(path, "w+b");
    }

    fseek(sSaveFile, 0, SEEK_END);
    int fileSize = ftell(sSaveFile);
    fseek(sSaveFile, 0, SEEK_SET);

    // Only read as many bytes as fit inside the buffer
    // or as many bytes as are in the file
    int bytesToRead = (fileSize < sizeof(FLASH_BASE)) ? fileSize : sizeof(FLASH_BASE);

    int bytesRead = fread(FLASH_BASE, 1, bytesToRead, sSaveFile);

    // Fill the buffer if the savefile was just created or smaller than the buffer itself
    for (int i = bytesRead; i < sizeof(FLASH_BASE); i++)
    {
        FLASH_BASE[i] = 0xFF;
    }
}

static void StoreSaveFile()
{
    if (sSaveFile != NULL)
    {
        fseek(sSaveFile, 0, SEEK_SET);
        fwrite(FLASH_BASE, 1, sizeof(FLASH_BASE), sSaveFile);
    }
}

void Platform_StoreSaveFile(void)
{
    StoreSaveFile();
}

void Platform_ReadFlash(u16 sectorNum, u32 offset, u8 *dest, u32 size)
{
    DBGPRINTF("ReadFlash(sectorNum=0x%04X,offset=0x%08X,size=0x%02X)\n",sectorNum,offset,size);
    FILE * savefile = fopen("pokeemerald.sav", "r+b");
    if (savefile == NULL)
    {
        puts("Error opening save file.");
        return;
    }
    if (fseek(savefile, (sectorNum << gFlash->sector.shift) + offset, SEEK_SET))
    {
        fclose(savefile);
        return;
    }
    if (fread(dest, 1, size, savefile) != size)
    {
        fclose(savefile);
        return;
    }
    fclose(savefile);
}

void Platform_QueueAudio(float *audioBuffer, s32 samplesPerFrame)
{
    if (!audioOutputReady || audioDeviceId == 0 || audioStream == NULL)
        return;

    s32 inputFloatCount = samplesPerFrame / (s32)sizeof(float);

    if (inputFloatCount <= 0)
        return;

    size_t requiredBytes = (size_t)inputFloatCount * sizeof(float);
    if (audioVolumeBufferFloats < (size_t)inputFloatCount)
    {
        float *newBuffer = realloc(audioVolumeBuffer, requiredBytes);
        if (newBuffer == NULL)
        {
            SDL_Log("Audio volume buffer allocation failed");
            return;
        }

        audioVolumeBuffer = newBuffer;
        audioVolumeBufferFloats = (size_t)inputFloatCount;
    }

    float *sourceBuffer = audioBuffer;
    if (audioVolume != 1.0f)
    {
        for (s32 i = 0; i < inputFloatCount; i++)
        {
            float scaled = audioBuffer[i] * audioVolume;
            if (scaled > 1.0f)
                scaled = 1.0f;
            else if (scaled < -1.0f)
                scaled = -1.0f;
            audioVolumeBuffer[i] = scaled;
        }

        sourceBuffer = audioVolumeBuffer;
    }

    if (SDL_AudioStreamPut(audioStream, sourceBuffer, requiredBytes) < 0)
    {
        SDL_Log("SDL_AudioStreamPut failed: %s", SDL_GetError());
        return;
    }

    int availableBytes = SDL_AudioStreamAvailable(audioStream);
    while (availableBytes > 0)
    {
        size_t chunkBytes = (size_t)availableBytes;
        if (audioOutputBufferBytes < chunkBytes)
        {
            Uint8 *newBuffer = realloc(audioOutputBuffer, chunkBytes);
            if (newBuffer == NULL)
            {
                SDL_Log("Audio output buffer allocation failed");
                return;
            }

            audioOutputBuffer = newBuffer;
            audioOutputBufferBytes = chunkBytes;
        }

        int bytesWritten = SDL_AudioStreamGet(audioStream, audioOutputBuffer, (int)chunkBytes);
        if (bytesWritten < 0)
        {
            SDL_Log("SDL_AudioStreamGet failed: %s", SDL_GetError());
            return;
        }

        if (bytesWritten > 0)
            SDL_QueueAudio(audioDeviceId, audioOutputBuffer, (Uint32)bytesWritten);

        availableBytes = SDL_AudioStreamAvailable(audioStream);
    }
}


static void CloseSaveFile()
{
    if (sSaveFile != NULL)
    {
        fclose(sSaveFile);
    }
}

// Key mappings
#define KEY_A_BUTTON      SDLK_z
#define KEY_B_BUTTON      SDLK_x
#define KEY_START_BUTTON  SDLK_RETURN
#define KEY_SELECT_BUTTON SDLK_BACKSLASH
#define KEY_L_BUTTON      SDLK_a
#define KEY_R_BUTTON      SDLK_s
#define KEY_DPAD_UP       SDLK_UP
#define KEY_DPAD_DOWN     SDLK_DOWN
#define KEY_DPAD_LEFT     SDLK_LEFT
#define KEY_DPAD_RIGHT    SDLK_RIGHT

#define HANDLE_KEYUP(key) \
case KEY_##key:  keys &= ~key; break;

#define HANDLE_KEYDOWN(key) \
case KEY_##key:  keys |= key; break;

static u16 keys;

void ProcessEvents(void)
{
    SDL_Event event;

    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
        case SDL_QUIT:
            isRunning = false;
            break;
        case SDL_KEYUP:
            switch (event.key.keysym.sym)
            {
            HANDLE_KEYUP(A_BUTTON)
            HANDLE_KEYUP(B_BUTTON)
            HANDLE_KEYUP(START_BUTTON)
            HANDLE_KEYUP(SELECT_BUTTON)
            HANDLE_KEYUP(L_BUTTON)
            HANDLE_KEYUP(R_BUTTON)
            HANDLE_KEYUP(DPAD_UP)
            HANDLE_KEYUP(DPAD_DOWN)
            HANDLE_KEYUP(DPAD_LEFT)
            HANDLE_KEYUP(DPAD_RIGHT)
            case SDLK_SPACE:
                if (speedUp)
                {
                    speedUp = false;
                    timeScale = 1.0;
                    if (audioOutputReady)
                    {
                        SDL_ClearQueuedAudio(audioDeviceId);
                        SDL_PauseAudioDevice(audioDeviceId, 0);
                    }
                }
                break;
            case SDLK_MINUS:
            case SDLK_KP_MINUS:
                AdjustAudioVolume(-0.1f);
                break;
            case SDLK_EQUALS:
            case SDLK_KP_PLUS:
                AdjustAudioVolume(0.1f);
                break;
            }
            break;
        case SDL_KEYDOWN:
            switch (event.key.keysym.sym)
            {
            HANDLE_KEYDOWN(A_BUTTON)
            HANDLE_KEYDOWN(B_BUTTON)
            HANDLE_KEYDOWN(START_BUTTON)
            HANDLE_KEYDOWN(SELECT_BUTTON)
            HANDLE_KEYDOWN(L_BUTTON)
            HANDLE_KEYDOWN(R_BUTTON)
            HANDLE_KEYDOWN(DPAD_UP)
            HANDLE_KEYDOWN(DPAD_DOWN)
            HANDLE_KEYDOWN(DPAD_LEFT)
            HANDLE_KEYDOWN(DPAD_RIGHT)
            case SDLK_r:
                if (event.key.keysym.mod & (KMOD_LCTRL | KMOD_RCTRL))
                {
                    DoSoftReset();
                }
                break;
            case SDLK_p:
                if (event.key.keysym.mod & (KMOD_LCTRL | KMOD_RCTRL))
                {
                    paused = !paused;
                }
                break;
            case SDLK_SPACE:
                if (!speedUp)
                {
                    speedUp = true;
                    timeScale = 5.0;
                    if (audioOutputReady)
                        SDL_PauseAudioDevice(audioDeviceId, 1);
                }
                break;
            case SDLK_MINUS:
            case SDLK_KP_MINUS:
                if (!event.key.repeat)
                    AdjustAudioVolume(-0.1f);
                break;
            case SDLK_EQUALS:
            case SDLK_KP_PLUS:
                if (!event.key.repeat)
                    AdjustAudioVolume(0.1f);
                break;
            }
            break;
        }
    }
}

#ifdef _WIN32
#define STICK_THRESHOLD 0.5f
u16 GetXInputKeys()
{
    XINPUT_STATE state;
    ZeroMemory(&state, sizeof(XINPUT_STATE));

    DWORD dwResult = XInputGetState(0, &state);
    u16 xinputKeys = 0;

    if (dwResult == ERROR_SUCCESS)
    {
        /* A */      xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_A) >> 12;
        /* B */      xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_X) >> 13;
        /* Start */  xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_START) >> 1;
        /* Select */ xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) >> 3;
        /* L */      xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) << 1;
        /* R */      xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) >> 1;
        /* Up */     xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP) << 6;
        /* Down */   xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) << 6;
        /* Left */   xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) << 3;
        /* Right */  xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) << 1;


        /* Control Stick */
        float xAxis = (float)state.Gamepad.sThumbLX / (float)SHRT_MAX;
        float yAxis = (float)state.Gamepad.sThumbLY / (float)SHRT_MAX;

        if (xAxis < -STICK_THRESHOLD) xinputKeys |= DPAD_LEFT;
        if (xAxis >  STICK_THRESHOLD) xinputKeys |= DPAD_RIGHT;
        if (yAxis < -STICK_THRESHOLD) xinputKeys |= DPAD_DOWN;
        if (yAxis >  STICK_THRESHOLD) xinputKeys |= DPAD_UP;


        /* Speedup */
        // Note: 'speedup' variable is only (un)set on keyboard input
        double oldTimeScale = timeScale;
        timeScale = (state.Gamepad.bRightTrigger > 0x80 || speedUp) ? 5.0 : 1.0;

        if (oldTimeScale != timeScale)
        {
            if (timeScale > 1.0)
            {
                if (audioOutputReady)
                    SDL_PauseAudioDevice(audioDeviceId, 1);
            }
            else
            {
                if (audioOutputReady)
                {
                    SDL_ClearQueuedAudio(audioDeviceId);
                    SDL_PauseAudioDevice(audioDeviceId, 0);
                }
            }
        }
    }

    return xinputKeys;
}
#endif // _WIN32

u16 Platform_GetKeyInput(void)
{
#ifdef _WIN32
    u16 gamepadKeys = GetXInputKeys();
    return (gamepadKeys != 0) ? gamepadKeys : keys;
#endif

    return keys;
}

void VDraw(SDL_Texture *texture)
{
    static uint16_t image[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static uint16_t previousImage[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static uint16_t topBgImage[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static uint16_t topSpriteImage[DISPLAY_WIDTH * DISPLAY_HEIGHT];

    memset(image, 0, sizeof(image));
#ifdef RENDERER_EASY_DRAW
    if (!DrawFrameTopLayers(image, topBgImage, topSpriteImage))
    {
        memset(topBgImage, 0, sizeof(topBgImage));
        memset(topSpriteImage, 0, sizeof(topSpriteImage));
        DrawFrame(image);
    }
#else
    memset(topBgImage, 0, sizeof(topBgImage));
    memset(topSpriteImage, 0, sizeof(topSpriteImage));
    DrawFrame(image);
#endif
    DrawVolumeOverlay(image);

    if (useShaderPipeline)
    {
        glBindTexture(GL_TEXTURE_2D, glPrevFrameTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, previousImage);

        glBindTexture(GL_TEXTURE_2D, glFrameTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, image);

        glBindTexture(GL_TEXTURE_2D, glTopBgTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, topBgImage);

        glBindTexture(GL_TEXTURE_2D, glTopSpriteTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, topSpriteImage);

        memcpy(previousImage, image, sizeof(image));
    }
    else
    {
        SDL_UpdateTexture(texture, NULL, image, DISPLAY_WIDTH * sizeof(Uint16));
    }

    REG_VCOUNT = 161; // prep for being in VBlank period
}

static bool ShaderFileExists(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return false;

    fclose(file);
    return true;
}

static char *LoadTextFile(const char *path)
{
    FILE *file = fopen(path, "rb");
    char *buffer;
    long size;

    if (file == NULL)
        return NULL;

    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= 0)
    {
        fclose(file);
        return NULL;
    }

    buffer = malloc((size_t)size + 1);
    if (buffer == NULL)
    {
        fclose(file);
        return NULL;
    }

    if (fread(buffer, 1, (size_t)size, file) != (size_t)size)
    {
        free(buffer);
        fclose(file);
        return NULL;
    }

    buffer[size] = '\0';
    fclose(file);
    return buffer;
}

static bool LoadShaderApi(void)
{
    pglCreateShader = (PFNGLCREATESHADERPROC)SDL_GL_GetProcAddress("glCreateShader");
    pglShaderSource = (PFNGLSHADERSOURCEPROC)SDL_GL_GetProcAddress("glShaderSource");
    pglCompileShader = (PFNGLCOMPILESHADERPROC)SDL_GL_GetProcAddress("glCompileShader");
    pglGetShaderiv = (PFNGLGETSHADERIVPROC)SDL_GL_GetProcAddress("glGetShaderiv");
    pglGetShaderInfoLog = (PFNGLGETSHADERINFOLOGPROC)SDL_GL_GetProcAddress("glGetShaderInfoLog");
    pglDeleteShader = (PFNGLDELETESHADERPROC)SDL_GL_GetProcAddress("glDeleteShader");
    pglCreateProgram = (PFNGLCREATEPROGRAMPROC)SDL_GL_GetProcAddress("glCreateProgram");
    pglAttachShader = (PFNGLATTACHSHADERPROC)SDL_GL_GetProcAddress("glAttachShader");
    pglLinkProgram = (PFNGLLINKPROGRAMPROC)SDL_GL_GetProcAddress("glLinkProgram");
    pglGetProgramiv = (PFNGLGETPROGRAMIVPROC)SDL_GL_GetProcAddress("glGetProgramiv");
    pglGetProgramInfoLog = (PFNGLGETPROGRAMINFOLOGPROC)SDL_GL_GetProcAddress("glGetProgramInfoLog");
    pglUseProgram = (PFNGLUSEPROGRAMPROC)SDL_GL_GetProcAddress("glUseProgram");
    pglDeleteProgram = (PFNGLDELETEPROGRAMPROC)SDL_GL_GetProcAddress("glDeleteProgram");
    pglGetUniformLocation = (PFNGLGETUNIFORMLOCATIONPROC)SDL_GL_GetProcAddress("glGetUniformLocation");
    pglUniform1i = (PFNGLUNIFORM1IPROC)SDL_GL_GetProcAddress("glUniform1i");
    pglUniform1f = (PFNGLUNIFORM1FPROC)SDL_GL_GetProcAddress("glUniform1f");
    pglUniform2f = (PFNGLUNIFORM2FPROC)SDL_GL_GetProcAddress("glUniform2f");
    pglActiveTexture = (PFNGLACTIVETEXTUREPROC)SDL_GL_GetProcAddress("glActiveTexture");
    pglGenVertexArrays = (PFNGLGENVERTEXARRAYSPROC)SDL_GL_GetProcAddress("glGenVertexArrays");
    pglBindVertexArray = (PFNGLBINDVERTEXARRAYPROC)SDL_GL_GetProcAddress("glBindVertexArray");
    pglDeleteVertexArrays = (PFNGLDELETEVERTEXARRAYSPROC)SDL_GL_GetProcAddress("glDeleteVertexArrays");
    pglGenBuffers = (PFNGLGENBUFFERSPROC)SDL_GL_GetProcAddress("glGenBuffers");
    pglBindBuffer = (PFNGLBINDBUFFERPROC)SDL_GL_GetProcAddress("glBindBuffer");
    pglBufferData = (PFNGLBUFFERDATAPROC)SDL_GL_GetProcAddress("glBufferData");
    pglDeleteBuffers = (PFNGLDELETEBUFFERSPROC)SDL_GL_GetProcAddress("glDeleteBuffers");
    pglVertexAttribPointer = (PFNGLVERTEXATTRIBPOINTERPROC)SDL_GL_GetProcAddress("glVertexAttribPointer");
    pglEnableVertexAttribArray = (PFNGLENABLEVERTEXATTRIBARRAYPROC)SDL_GL_GetProcAddress("glEnableVertexAttribArray");
    pglDrawArrays = (PokeGlDrawArraysProc)SDL_GL_GetProcAddress("glDrawArrays");

    return pglCreateShader != NULL
        && pglShaderSource != NULL
        && pglCompileShader != NULL
        && pglGetShaderiv != NULL
        && pglGetShaderInfoLog != NULL
        && pglDeleteShader != NULL
        && pglCreateProgram != NULL
        && pglAttachShader != NULL
        && pglLinkProgram != NULL
        && pglGetProgramiv != NULL
        && pglGetProgramInfoLog != NULL
        && pglUseProgram != NULL
        && pglDeleteProgram != NULL
        && pglGetUniformLocation != NULL
        && pglUniform1i != NULL
        && pglUniform1f != NULL
        && pglUniform2f != NULL
        && pglActiveTexture != NULL
        && pglGenVertexArrays != NULL
        && pglBindVertexArray != NULL
        && pglDeleteVertexArrays != NULL
        && pglGenBuffers != NULL
        && pglBindBuffer != NULL
        && pglBufferData != NULL
        && pglDeleteBuffers != NULL
        && pglVertexAttribPointer != NULL
        && pglEnableVertexAttribArray != NULL
        && pglDrawArrays != NULL;
}

static GLuint CompileShader(GLenum type, const char *source, const char *label)
{
    GLint status = 0;
    GLuint shader = pglCreateShader(type);
    if (shader == 0)
        return 0;

    pglShaderSource(shader, 1, &source, NULL);
    pglCompileShader(shader);
    pglGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == GL_FALSE)
    {
        char logBuffer[1024];
        pglGetShaderInfoLog(shader, sizeof(logBuffer), NULL, logBuffer);
        DBGPRINTF("%s compile error: %s\n", label, logBuffer);
        pglDeleteShader(shader);
        return 0;
    }

    return shader;
}

static bool TryInitShaderPipeline(void)
{
    static const char *vertexSource =
        "#version 330 core\n"
        "layout(location = 0) in vec2 aPos;\n"
        "layout(location = 1) in vec2 aTexCoord;\n"
        "out vec2 vTexCoord;\n"
        "void main()\n"
        "{\n"
        "    vTexCoord = aTexCoord;\n"
        "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "}\n";
    static const GLfloat quadVertices[] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
    };
    GLint linkStatus = 0;
    GLuint vertexShader;
    GLuint fragmentShader;
    char *fragmentSource;

    if (!ShaderFileExists(fragmentShaderPath))
    {
        DBGPRINTF("Shader file not found at '%s', falling back to nearest\n", fragmentShaderPath);
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    sdlGlContext = SDL_GL_CreateContext(sdlWindow);
    if (sdlGlContext == NULL)
    {
        DBGPRINTF("Failed to create GL context: %s\n", SDL_GetError());
        return false;
    }

    if (!LoadShaderApi())
    {
        DBGPRINTF("Required GL shader API unavailable, falling back to nearest\n");
        ShutdownShaderPipeline();
        return false;
    }

    fragmentSource = LoadTextFile(fragmentShaderPath);
    if (fragmentSource == NULL)
    {
        DBGPRINTF("Could not read shader file '%s'\n", fragmentShaderPath);
        ShutdownShaderPipeline();
        return false;
    }

    vertexShader = CompileShader(GL_VERTEX_SHADER, vertexSource, "Vertex shader");
    fragmentShader = CompileShader(GL_FRAGMENT_SHADER, fragmentSource, "Fragment shader");
    free(fragmentSource);
    if (vertexShader == 0 || fragmentShader == 0)
    {
        if (vertexShader != 0)
            pglDeleteShader(vertexShader);
        if (fragmentShader != 0)
            pglDeleteShader(fragmentShader);
        ShutdownShaderPipeline();
        return false;
    }

    glProgram = pglCreateProgram();
    if (glProgram == 0)
    {
        pglDeleteShader(vertexShader);
        pglDeleteShader(fragmentShader);
        ShutdownShaderPipeline();
        return false;
    }

    pglAttachShader(glProgram, vertexShader);
    pglAttachShader(glProgram, fragmentShader);
    pglLinkProgram(glProgram);
    pglGetProgramiv(glProgram, GL_LINK_STATUS, &linkStatus);
    pglDeleteShader(vertexShader);
    pglDeleteShader(fragmentShader);
    if (linkStatus == GL_FALSE)
    {
        char logBuffer[1024];
        pglGetProgramInfoLog(glProgram, sizeof(logBuffer), NULL, logBuffer);
        DBGPRINTF("Shader link error: %s\n", logBuffer);
        ShutdownShaderPipeline();
        return false;
    }

    glMainTextureUniform = pglGetUniformLocation(glProgram, "uMainTex");
    glPrevFrameTextureUniform = pglGetUniformLocation(glProgram, "uPrevFrameTex");
    glTopBgTextureUniform = pglGetUniformLocation(glProgram, "uTopBgTex");
    glTopSpriteTextureUniform = pglGetUniformLocation(glProgram, "uTopSpriteTex");
    glRenderResolutionUniform = pglGetUniformLocation(glProgram, "uRenderResolution");
    glSourceResolutionUniform = pglGetUniformLocation(glProgram, "uSourceResolution");
    glOutputResolutionUniform = pglGetUniformLocation(glProgram, "uOutputResolution");
    glTimeUniform = pglGetUniformLocation(glProgram, "uTime");
    glFrameUniform = pglGetUniformLocation(glProgram, "uFrame");
    glScaleUniform = pglGetUniformLocation(glProgram, "uScale");
    glAudioVolumeUniform = pglGetUniformLocation(glProgram, "uAudioVolume");
    glSpeedUniform = pglGetUniformLocation(glProgram, "uSpeed");

    pglGenVertexArrays(1, &glQuadVao);
    pglBindVertexArray(glQuadVao);
    pglGenBuffers(1, &glQuadVbo);
    pglBindBuffer(GL_ARRAY_BUFFER, glQuadVbo);
    pglBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    pglEnableVertexAttribArray(0);
    pglVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)0);
    pglEnableVertexAttribArray(1);
    pglVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void *)(2 * sizeof(GLfloat)));
    pglBindBuffer(GL_ARRAY_BUFFER, 0);
    pglBindVertexArray(0);

    glGenTextures(1, &glFrameTexture);
    glBindTexture(GL_TEXTURE_2D, glFrameTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB5_A1, DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, NULL);

    glGenTextures(1, &glPrevFrameTexture);
    glBindTexture(GL_TEXTURE_2D, glPrevFrameTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB5_A1, DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, NULL);

    glGenTextures(1, &glTopBgTexture);
    glBindTexture(GL_TEXTURE_2D, glTopBgTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB5_A1, DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, NULL);

    glGenTextures(1, &glTopSpriteTexture);
    glBindTexture(GL_TEXTURE_2D, glTopSpriteTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB5_A1, DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, NULL);

    if (glMainTextureUniform >= 0)
    {
        pglUseProgram(glProgram);
        pglUniform1i(glMainTextureUniform, 0);
        if (glPrevFrameTextureUniform >= 0)
            pglUniform1i(glPrevFrameTextureUniform, 1);
        if (glTopBgTextureUniform >= 0)
            pglUniform1i(glTopBgTextureUniform, 2);
        if (glTopSpriteTextureUniform >= 0)
            pglUniform1i(glTopSpriteTextureUniform, 3);
        pglUseProgram(0);
    }

    shaderStartTicks = SDL_GetTicks();
    shaderFrameCounter = 0;

    SDL_GL_SetSwapInterval(1);
    return true;
}

static void RenderFrameWithShader(void)
{
    int windowW;
    int windowH;
    float scaleX;
    float scaleY;
    float elapsedSeconds;

    SDL_GetWindowSize(sdlWindow, &windowW, &windowH);
    glViewport(0, 0, windowW, windowH);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    pglUseProgram(glProgram);
    pglActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, glFrameTexture);
    pglActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, glPrevFrameTexture);
    pglActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, glTopBgTexture);
    pglActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, glTopSpriteTexture);

    scaleX = (float)windowW / (float)DISPLAY_WIDTH;
    scaleY = (float)windowH / (float)DISPLAY_HEIGHT;
    elapsedSeconds = (float)(SDL_GetTicks() - shaderStartTicks) * 0.001f;

    if (glRenderResolutionUniform >= 0)
        pglUniform2f(glRenderResolutionUniform, (float)windowW, (float)windowH);
    if (glSourceResolutionUniform >= 0)
        pglUniform2f(glSourceResolutionUniform, (float)DISPLAY_WIDTH, (float)DISPLAY_HEIGHT);
    if (glOutputResolutionUniform >= 0)
        pglUniform2f(glOutputResolutionUniform, (float)windowW, (float)windowH);
    if (glTimeUniform >= 0)
        pglUniform1f(glTimeUniform, elapsedSeconds);
    if (glFrameUniform >= 0)
        pglUniform1i(glFrameUniform, shaderFrameCounter);
    if (glScaleUniform >= 0)
        pglUniform2f(glScaleUniform, scaleX, scaleY);
    if (glAudioVolumeUniform >= 0)
        pglUniform1f(glAudioVolumeUniform, audioVolume);
    if (glSpeedUniform >= 0)
        pglUniform1f(glSpeedUniform, (float)timeScale);
    
    pglBindVertexArray(glQuadVao);
    pglDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    pglBindVertexArray(0);
    pglActiveTexture(GL_TEXTURE0);

    pglUseProgram(0);
    shaderFrameCounter++;
}

static void ShutdownShaderPipeline(void)
{
    if (glQuadVbo != 0)
    {
        pglDeleteBuffers(1, &glQuadVbo);
        glQuadVbo = 0;
    }

    if (glQuadVao != 0)
    {
        pglDeleteVertexArrays(1, &glQuadVao);
        glQuadVao = 0;
    }

    if (glTopSpriteTexture != 0)
    {
        glDeleteTextures(1, &glTopSpriteTexture);
        glTopSpriteTexture = 0;
    }

    if (glTopBgTexture != 0)
    {
        glDeleteTextures(1, &glTopBgTexture);
        glTopBgTexture = 0;
    }

    if (glPrevFrameTexture != 0)
    {
        glDeleteTextures(1, &glPrevFrameTexture);
        glPrevFrameTexture = 0;
    }

    if (glFrameTexture != 0)
    {
        glDeleteTextures(1, &glFrameTexture);
        glFrameTexture = 0;
    }

    if (glProgram != 0)
    {
        pglDeleteProgram(glProgram);
        glProgram = 0;
    }

    glMainTextureUniform = -1;
    glPrevFrameTextureUniform = -1;
    glTopBgTextureUniform = -1;
    glTopSpriteTextureUniform = -1;
    glRenderResolutionUniform = -1;
    glSourceResolutionUniform = -1;
    glOutputResolutionUniform = -1;
    glTimeUniform = -1;
    glFrameUniform = -1;
    glScaleUniform = -1;
    glAudioVolumeUniform = -1;
    glSpeedUniform = -1;
    shaderStartTicks = 0;
    shaderFrameCounter = 0;

    if (sdlGlContext != NULL)
    {
        SDL_GL_DeleteContext(sdlGlContext);
        sdlGlContext = NULL;
    }
}

int DoMain(void *data)
{
    AgbMain();
}

static char *TrimWhitespace(char *text)
{
    char *end;

    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        text++;

    if (*text == '\0')
        return text;

    end = text + strlen(text) - 1;
    while (end > text && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
    {
        *end = '\0';
        end--;
    }

    return text;
}

static bool ParseBoolValue(const char *value, bool defaultValue)
{
    if (SDL_strcasecmp(value, "1") == 0 || SDL_strcasecmp(value, "true") == 0 || SDL_strcasecmp(value, "yes") == 0 || SDL_strcasecmp(value, "on") == 0)
        return true;

    if (SDL_strcasecmp(value, "0") == 0 || SDL_strcasecmp(value, "false") == 0 || SDL_strcasecmp(value, "no") == 0 || SDL_strcasecmp(value, "off") == 0)
        return false;

    return defaultValue;
}

static void LoadPcConfig(const char *path, struct PcConfig *config)
{
    FILE *file = fopen(path, "rb");
    char line[512];

    if (file == NULL)
        return;

    while (fgets(line, sizeof(line), file) != NULL)
    {
        char *trimmed = TrimWhitespace(line);
        char *equals;
        char *key;
        char *value;

        if (*trimmed == '\0' || *trimmed == '#' || *trimmed == ';')
            continue;

        equals = strchr(trimmed, '=');
        if (equals == NULL)
            continue;

        *equals = '\0';
        key = TrimWhitespace(trimmed);
        value = TrimWhitespace(equals + 1);

        if (SDL_strcasecmp(key, "enable_shader") == 0)
        {
            config->enableShader = ParseBoolValue(value, config->enableShader);
        }
        else if (SDL_strcasecmp(key, "window_scale") == 0)
        {
            unsigned int parsedScale = (unsigned int)strtoul(value, NULL, 10);
            if (parsedScale >= 1 && parsedScale <= 10)
                config->windowScale = parsedScale;
        }
        else if (SDL_strcasecmp(key, "shader_path") == 0)
        {
            strncpy(config->shaderPath, value, sizeof(config->shaderPath) - 1);
            config->shaderPath[sizeof(config->shaderPath) - 1] = '\0';
        }
        else if (SDL_strcasecmp(key, "enable_audio") == 0)
        {
            config->enableAudio = ParseBoolValue(value, config->enableAudio);
        }
        else if (SDL_strcasecmp(key, "audio_sample_rate") == 0)
        {
            long parsedRate = strtol(value, NULL, 10);
            if (parsedRate >= 8000 && parsedRate <= 192000)
                config->audioSampleRate = (int)parsedRate;
        }
        else if (SDL_strcasecmp(key, "audio_buffer_samples") == 0)
        {
            unsigned int parsedSamples = (unsigned int)strtoul(value, NULL, 10);
            if (parsedSamples >= 128 && parsedSamples <= 8192)
                config->audioBufferSamples = parsedSamples;
        }
        else if (SDL_strcasecmp(key, "audio_volume") == 0)
        {
            float parsedVolume = strtof(value, NULL);
            if (parsedVolume >= 0.0f && parsedVolume <= 4.0f)
                config->audioVolume = parsedVolume;
        }
    }

    fclose(file);
}

void VBlankIntrWait(void)
{
    SDL_AtomicSet(&isFrameAvailable, 1);
    SDL_SemWait(vBlankSemaphore);
}

u8 BinToBcd(u8 bin)
{
    int placeCounter = 1;
    u8 out = 0;
    do
    {
        out |= (bin % 10) * placeCounter;
        placeCounter *= 16;
    }
    while ((bin /= 10) > 0);

    return out;
}

void Platform_GetStatus(struct SiiRtcInfo *rtc)
{
    rtc->status = internalClock.status;
}

void Platform_SetStatus(struct SiiRtcInfo *rtc)
{
    internalClock.status = rtc->status;
}

static void UpdateInternalClock(void)
{
    time_t rawTime = time(NULL);
    struct tm *time = localtime(&rawTime);

    internalClock.year = BinToBcd(time->tm_year - 100);
    internalClock.month = BinToBcd(time->tm_mon) + 1;
    internalClock.day = BinToBcd(time->tm_mday);
    internalClock.dayOfWeek = BinToBcd(time->tm_wday);
    internalClock.hour = BinToBcd(time->tm_hour);
    internalClock.minute = BinToBcd(time->tm_min);
    internalClock.second = BinToBcd(time->tm_sec);
}

static void ShowVolumeNotice(float volume)
{
    int percent = (int)(volume * 100.0f + 0.5f);

    if (percent < 0)
        percent = 0;
    if (percent > 400)
        percent = 400;

    SDL_snprintf(volumeNoticeText, sizeof(volumeNoticeText), "VOL %d%%", percent);
    volumeNoticeUntil = SDL_GetTicks() + 1500;
    showVolumeNotice = true;

    SDL_SetWindowTitle(sdlWindow, volumeNoticeText);
}

static void AdjustAudioVolume(float delta)
{
    float newVolume = audioVolume + delta;

    if (newVolume < 0.0f)
        newVolume = 0.0f;
    if (newVolume > 4.0f)
        newVolume = 4.0f;

    audioVolume = newVolume;
    ShowVolumeNotice(audioVolume);
}

static void DrawOverlayBox(uint16_t *pixels, int x, int y, int w, int h, uint16_t color)
{
    for (int row = 0; row < h; row++)
    {
        int py = y + row;
        if (py < 0 || py >= DISPLAY_HEIGHT)
            continue;

        for (int col = 0; col < w; col++)
        {
            int px = x + col;
            if (px < 0 || px >= DISPLAY_WIDTH)
                continue;

            pixels[py * DISPLAY_WIDTH + px] = color;
        }
    }
}

static const uint8_t *GetOverlayGlyph(char ch)
{
    switch (ch)
    {
    case 'V':
    {
        static const uint8_t glyph[] = {0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04, 0x04};
        return glyph;
    }
    case 'O':
    {
        static const uint8_t glyph[] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
        return glyph;
    }
    case 'L':
    {
        static const uint8_t glyph[] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
        return glyph;
    }
    case '+':
    {
        static const uint8_t glyph[] = {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00};
        return glyph;
    }
    case '-':
    {
        static const uint8_t glyph[] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
        return glyph;
    }
    case '%':
    {
        static const uint8_t glyph[] = {0x11, 0x02, 0x04, 0x08, 0x10, 0x11, 0x00};
        return glyph;
    }
    case '0':
    {
        static const uint8_t glyph[] = {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
        return glyph;
    }
    case '1':
    {
        static const uint8_t glyph[] = {0x04, 0x0C, 0x14, 0x04, 0x04, 0x04, 0x1F};
        return glyph;
    }
    case '2':
    {
        static const uint8_t glyph[] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
        return glyph;
    }
    case '3':
    {
        static const uint8_t glyph[] = {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
        return glyph;
    }
    case '4':
    {
        static const uint8_t glyph[] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
        return glyph;
    }
    case '5':
    {
        static const uint8_t glyph[] = {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
        return glyph;
    }
    case '6':
    {
        static const uint8_t glyph[] = {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
        return glyph;
    }
    case '7':
    {
        static const uint8_t glyph[] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
        return glyph;
    }
    case '8':
    {
        static const uint8_t glyph[] = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
        return glyph;
    }
    case '9':
    {
        static const uint8_t glyph[] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E};
        return glyph;
    }
    case ' ':
    default:
    {
        static const uint8_t glyph[] = {0, 0, 0, 0, 0, 0, 0};
        return glyph;
    }
    }
}

static void DrawOverlayChar(uint16_t *pixels, int x, int y, char ch, uint16_t color)
{
    const uint8_t *glyph = GetOverlayGlyph(ch);

    for (int row = 0; row < 7; row++)
    {
        for (int col = 0; col < 5; col++)
        {
            if (glyph[row] & (1 << (4 - col)))
            {
                int px = x + col;
                int py = y + row;
                if (px >= 0 && px < DISPLAY_WIDTH && py >= 0 && py < DISPLAY_HEIGHT)
                    pixels[py * DISPLAY_WIDTH + px] = color;
            }
        }
    }
}

static int OverlayTextWidth(const char *text)
{
    int length = 0;

    while (text[length] != '\0')
        length++;

    return (length * 6) - 1;
}

static void DrawOverlayText(uint16_t *pixels, int x, int y, const char *text, uint16_t color)
{
    int cursorX = x;

    for (int i = 0; text[i] != '\0'; i++)
    {
        DrawOverlayChar(pixels, cursorX, y, text[i], color);
        cursorX += 6;
    }
}

static void DrawVolumeOverlay(uint16_t *pixels)
{
    int boxX;
    int boxY = 6;
    int boxW;
    int boxH = 18;
    int textX;

    if (!showVolumeNotice)
        return;

    if (SDL_GetTicks() >= volumeNoticeUntil)
    {
        showVolumeNotice = false;
        return;
    }

    boxX = 6;
    boxW = OverlayTextWidth(volumeNoticeText) + 12;
    if (boxW < 58)
        boxW = 58;

    DrawOverlayBox(pixels, boxX, boxY, boxW, boxH, RGB555(0, 0, 0));
    DrawOverlayBox(pixels, boxX, boxY, boxW, 1, RGB555_WHITE);
    DrawOverlayBox(pixels, boxX, boxY + boxH - 1, boxW, 1, RGB555_WHITE);
    DrawOverlayBox(pixels, boxX, boxY, 1, boxH, RGB555_WHITE);
    DrawOverlayBox(pixels, boxX + boxW - 1, boxY, 1, boxH, RGB555_WHITE);

    textX = boxX + 6;
    DrawOverlayText(pixels, textX, boxY + 4, volumeNoticeText, RGB555_WHITE);
}

void Platform_GetDateTime(struct SiiRtcInfo *rtc)
{
    UpdateInternalClock();

    rtc->year = internalClock.year;
    rtc->month = internalClock.month;
    rtc->day = internalClock.day;
    rtc->dayOfWeek = internalClock.dayOfWeek;
    rtc->hour = internalClock.hour;
    rtc->minute = internalClock.minute;
    rtc->second = internalClock.second;
    DBGPRINTF("GetDateTime: %d-%02d-%02d %02d:%02d:%02d\n", ConvertBcdToBinary(rtc->year),
                                                         ConvertBcdToBinary(rtc->month),
                                                         ConvertBcdToBinary(rtc->day),
                                                         ConvertBcdToBinary(rtc->hour),
                                                         ConvertBcdToBinary(rtc->minute),
                                                         ConvertBcdToBinary(rtc->second));
}

void Platform_SetDateTime(struct SiiRtcInfo *rtc)
{
    internalClock.month = rtc->month;
    internalClock.day = rtc->day;
    internalClock.dayOfWeek = rtc->dayOfWeek;
    internalClock.hour = rtc->hour;
    internalClock.minute = rtc->minute;
    internalClock.second = rtc->second;
}

void Platform_GetTime(struct SiiRtcInfo *rtc)
{
    UpdateInternalClock();

    rtc->hour = internalClock.hour;
    rtc->minute = internalClock.minute;
    rtc->second = internalClock.second;
    DBGPRINTF("GetTime: %02d:%02d:%02d\n", ConvertBcdToBinary(rtc->hour),
                                        ConvertBcdToBinary(rtc->minute),
                                        ConvertBcdToBinary(rtc->second));
}

void Platform_SetTime(struct SiiRtcInfo *rtc)
{
    internalClock.hour = rtc->hour;
    internalClock.minute = rtc->minute;
    internalClock.second = rtc->second;
}

void Platform_SetAlarm(u8 *alarmData)
{
    // TODO
}

void SoftReset(u32 resetFlags)
{
    puts("Soft Reset called. Exiting.");
    exit(0);
}

#endif