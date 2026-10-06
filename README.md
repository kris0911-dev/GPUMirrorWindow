# GPU Mirror Window

The main window is one process. Each tab is another process, and that process has its own window. The host does not embed those windows. It copies the picture each renderer is already showing.

```
mirror_host                          mirror_renderer  (one process per tab)
  HWND / NSWindow                      its own HWND / NSWindow
  tab strip, copy of one renderer        Video, Image, or Controls
        |                                      |
        |  anonymous pipe / unix socket        |  draws into its window
        |<--------- hello, frame --------------+
        |                                      |
        |         shared texture               |
        +----------- the same pixels ----------+
                     Windows: D3D11 NT handle + keyed mutex
                     macOS:   IOSurface
```

This is the same process split as [QMLBrowser](https://github.com/kris0911-dev/QMLBrowser): the UI process owns the mirror, and a crashed tab takes down only its renderer. QMLBrowser embeds the renderer as a child window. Here the renderer window stays independent, and the mirror is a copy of its pixels, so a video frame is inside the host's own swap chain.

| | Windows | macOS |
|---|---|---|
| Renderer window | `HWND`, Direct2D | `NSWindow`, Core Graphics |
| What it shows | Media Foundation video, a WIC photo, or buttons | AVFoundation video, the same photo, or buttons |
| Shared surface | Renderer creates a D3D11 texture (`SHARED_NTHANDLE` + keyed mutex). The host opens it by name. | Host creates an `IOSurface`. The renderer looks it up by id. |
| Sync | renderer acquires key 0, host acquires key 1 | renderer publishes the frame, host blits, then sends `ack` |
| IPC | TCP `127.0.0.1:47631`. The renderer connects when you start it. | unix socket `/tmp/gpumirror_host.sock` |

The renderer window is a normal window. Its video, photo, and buttons are laid out in the client area, so resizing the window moves those controls instead of stretching a picture. The host copies that same picture and keeps its aspect ratio.

The host list starts empty. Start `mirror_video`, `mirror_image`, and `mirror_controls` yourself; the host does not launch them. As each one connects, its name is added at the top of the host window. Nothing is mirrored until you select a row. Click more than one and the host shows those pictures together. Click a selected row again to let it go. Closing the host leaves the renderer windows running.

| App | What it draws |
|---|---|
| mirror_video | `assets/flower.mp4`, with Play / Pause and a timeline. |
| mirror_image | `assets/photo.jpg`, with Fit and Actual size. |
| mirror_controls | Start, Stop, Reset, and Add note. The status line changes when you click. |

Click the buttons on a renderer window, or on that picture inside the host. The host's list is its own. Those pixels never come from a renderer.

## Build

Windows needs Visual Studio 2022 or later with the Desktop development with C++ workload, and CMake. No Qt.

```bat
build.bat
build\bin\mirror_host.exe
```

`mirror_host.exe` only listens. Start `mirror_video.exe`, `mirror_image.exe`, and `mirror_controls.exe` from `build\bin` when you want them. The photo and video are copied to `build\bin\assets`. Each name shows up in the host list when that process connects. Click a name, or press 1, 2, or 3, to mirror it. Click it again to stop mirroring. Closing a renderer window removes only that row. Resize and move either window the way you would any other window.

macOS needs Xcode and CMake. Deployment target is 11.0.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/bin/mirror_host
```

## Messages

`src/protocol.h`. Each write is one `Message`.

| type | who sends it | meaning |
|---|---|---|
| hello | renderer | process id, after the shared surface is open |
| frame | renderer | one finished frame |
| ack | host, macOS only | the blit of that frame finished, so the renderer may write the surface again |
| shutdown | host | exit |
| pointer | host | a click in the mirrored picture; `width` and `height` are surface pixels, `generation` is down or up |

On Windows the keyed mutex is the real frame sync. `frame` is still sent so the host can show a frame rate. The renderer window keeps running if the host is closed.
