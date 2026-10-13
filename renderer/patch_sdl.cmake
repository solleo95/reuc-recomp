# SDL3-3.2.0 can leave fullscreen while failed window creation has no HWND data.
set(sdl_window_file "${sdl3_SOURCE_DIR}/src/video/windows/SDL_windowswindow.c")
file(READ "${sdl_window_file}" sdl_window_source)
set(sdl_window_before [=[SDL_FullscreenResult WIN_SetWindowFullscreen(SDL_VideoDevice *_this, SDL_Window *window, SDL_VideoDisplay *display, SDL_FullscreenOp fullscreen)
{
#if !defined(SDL_PLATFORM_XBOXONE) && !defined(SDL_PLATFORM_XBOXSERIES)
]=])
set(sdl_window_after "${sdl_window_before}")
string(APPEND sdl_window_after [=[    if (!window->internal) {
        if (fullscreen == SDL_FULLSCREEN_OP_LEAVE) {
            return SDL_FULLSCREEN_SUCCEEDED;
        }
        SDL_SetError("Windows window is not initialized");
        return SDL_FULLSCREEN_FAILED;
    }
]=])
string(FIND "${sdl_window_source}" "${sdl_window_after}" sdl_window_patched)
if(sdl_window_patched LESS 0)
  string(FIND "${sdl_window_source}" "${sdl_window_before}" sdl_window_original)
  if(sdl_window_original LESS 0)
    message(FATAL_ERROR "SDL window lifecycle patch does not match the pinned source")
  endif()
  string(REPLACE "${sdl_window_before}" "${sdl_window_after}" sdl_window_source "${sdl_window_source}")
  file(WRITE "${sdl_window_file}" "${sdl_window_source}")
endif()
# Preserve the original SetProp error across rollback cleanup calls.
set(sdl_error_before [=[        ReleaseDC(hwnd, data->hdc);
        SDL_free(data);
        return WIN_SetError("SetProp() failed");]=])
set(sdl_error_after [=[        const DWORD setprop_error = GetLastError();
        ReleaseDC(hwnd, data->hdc);
        SDL_free(data);
        SetLastError(setprop_error);
        return WIN_SetError("SetProp() failed");]=])
string(FIND "${sdl_window_source}" "${sdl_error_after}" sdl_error_patched)
if(sdl_error_patched LESS 0)
  string(FIND "${sdl_window_source}" "${sdl_error_before}" sdl_error_original)
  if(sdl_error_original LESS 0)
    message(FATAL_ERROR "SDL SetProp rollback patch does not match the pinned source")
  endif()
  string(REPLACE "${sdl_error_before}" "${sdl_error_after}" sdl_window_source "${sdl_window_source}")
  file(WRITE "${sdl_window_file}" "${sdl_window_source}")
endif()
