# BlueEngine, a Python Engine.
Least Version:
Version 0.05 (aka Part: ii)

By SrRotiehc, 2026.
To instructions on how to install, please check the bottom of the readme at "compile".

(This is a experimental version with LOTS of resources missing)

```python
import BlueEngine as be    # the python version of the .so must be the same as the one used, the .so also needs to be in the same folder as the project, it is recommended to not rename the .so
```
## Window and Loop

| Function | Description |
|---|---|
| `start(width=1600, height=800, title="Blue Engine")` `*` | Create the window and the context alongside with other basic parameters, it will give an error if it's called twice. |
| `quit()` `*` | Closes the window and clear the cache, can be called twice even though it is not recommended. |
| `running()` → `bool` `*` | `True` Function that checks if the instance is running, will return true if so unless `stop()`/`quit()` are called. |
| `stop()` `*` | Ask to stop (`running()` becomes `False`). |
| `windowClosed()` → `bool` `*` | `True` Only when the instance is closed (X, Alt F4...). |
| `windowTitle(title)` | Changes the window title. |
| `pollEvents()` `*` | Read the events frame by frame. To use it just call it once per frame in the main loop, at the start of it. |
| `deltaTime()` → `float` `*` | Seconds before the last two `pollEvents()` (limited to 0.1s). |

## Inputs

Keyboard input names are the same as the SDL: `"w"`, `"space"`, `"escape"`, `"left shift"`, `"up"`… (invalid name → `ValueError`).

| Function | Description |
|---|---|
| `keyDown(key)` → `bool` `*` | `True` While it is being hold. |
| `keyPressed(key)` → `bool` `*` | `True` Only in the frame that it was pressed (ignores auto-repeat). |
| `mousePos()` → `(dx, dy)` `*` | How much the mouse has moved (There isn't a function for checking the mouse position in the screen for now). |
| `setMouseCap(on)` | `True` Hides the mouse and starts checking only it movement. |
| `mouseCap()` → `bool` | Returns if the mouse is being captured. |

## Draw/Render

Frame order: `clear(...)` → `model.render()` of each model → `flip()`.

| Function | Description |
|---|---|
| `clear(color=(0,0,0))` | Clear the screen and set a color for the window (prevents render artefacts). |
| `flip()` | Shows what was dranw in the screen (use it at the end of each render loop). |
| `setShade(color)` | Global RGB color scheme for everything. `(255,255,255)` = no changes. |
| `setWireframe(on)` | `True` draw the lines of each face (recommended only for debug). |

## Camera — object `BlueEngine.camera`

If called with values, apply them; If not, it returns the base values.

| Function | Description |
|---|---|
| `camera.pos()` / `pos(x, y, z)` / `pos((x, y, z))` | Starter camera position (starts at): `(0, 0, 5)`. |
| `camera.yaw()` / `yaw(degrees)` | Horizontal rotation. Positive turns to the right. |
| `camera.pitch()` / `pitch(degrees)` | Vertical rotation, limited to ±89°. Positive looks down. |
| `camera.fov()` / `fov(degrees)` | FOV (60 if not changed). |
| `camera.rotate(yaw=0, pitch=0)` | **Add** the values to the actual rotations (good for mouse looking). |

To walk in the direction it is looking at a horizontal plane (needs the math module for python):
`front = (sin(yaw), 0, -cos(yaw))` and `right = (cos(yaw), 0, sin(yaw))`, with `yaw` in radians.

## Models

```python
loadModel(obj="", texture="", pos=(0,0,0), scale=1.0, rotation=(0,0,0), transparent=False) -> Model
```

| `obj` | `texture` | Result |
|---|---|---|
| ✔ | ✔ | Model `.obj` with texture. |
| ✔ | — | Model `.obj` without texture (color of `setShade`). |
| — | ✔ | **2D Plane** (“paper”): height of 1, width in proportion as the image, in the XY plane turned to +Z, visible on both sides. |
| — | — | **Blank Plane** 1×1, no texture, with `setShade` color. With `transparent=True` turns invisible. |

- `transparent=True` Allows the alpha channel on a .png texture. Without texture, it becomes invisible.
- `scale` Accepts a single number or `(x, y, z)`. Positive multiplies, negative divides (`-2` = half), `0` stays the same.
- Erros (`RuntimeError`): `start()` wasn't called, or `.obj`/texture weren't able to load.
- Just `loadModel()` without a model or texture does not create an error, it just creates a blank plane.

### Methods of `Model`

As in the camera, with values it uses them, without it returns.

| Method | Description |
|---|---|
| `pos()` / `pos(x, y, z)` / `pos((x, y, z))` | Position. |
| `scale()` / `scale(x, y, z)` / `scale(single_value_or(x,y,z))` | Scale (the read value is alredy the final result: `scale(-2)` reads `0.5`). |
| `rotation()` / `rotation(x, y, z)` / `rotation((x, y, z))` | Rotation in degrees. |
| `move(x=0, y=0, z=0)` | Adds to the position. |
| `rotate(x=0, y=0, z=0)` | Adds to the rotation. |
| `transparent()` / `transparent(on)` | Reads/Turn on or off the transparency. |
| `render()` | Draws the model (between `clear()` and `flip()`). |
| `unload()` | Clears the GPU cache of the models and texture (alredy happens when the engine is closed). |

## Types of models to use

- **Model `.obj`:** lines `v`, `vt` e `f` (formats `v`, `v/vt`, `v/vt/vn` e `v//vn`, so is the negative ones). Faces with 4 or more vertices are turned into triangles. Normals and materials (`.mtl`) are ignored.
- **Texture:** any image format that Pillow can open (PNG, JPG…), converts to RGBA.

## Direct Functions (not recommended to use)

`_start`, `_quit`, `_pollRaw`, `_keyHeld`, `_scancode`, `_ticks` — pure SDL layers that `main.py` uses to build the functions above.

---

## Compile
To install and use the engine, you must compile it by hand following the tutorial bellow or simply install the .so with the corresponding version of your python and put it on the same folder of the project. For any problems trying to launch the engine, please check bellow.

Dependencies (only for linux by now)
(Debian/Ubuntu): `sudo apt install g++ libsdl2-dev libgl-dev` 
(Fedora): `dnf install gcc-c++ SDL2-devel mesa-libGL-devel python3-devel`
(Arch): `pacman -S base-devel sdl2-compat mesa python python-pip`
(openSUSE): `zypper install gcc-c++ SDL2-devel Mesa-libGL-devel python3-devel`
And for each Python version:
`python3.X -m pip install pybind11 numpy pillow` (numpy and Pillow are necessary in order to run).
You may need to create a .venv on the folder of your project with the dependencies in order to run

```bash
bash build.sh                       # python3.11, 3.12, 3.13 e 3.14
bash build.sh python3.12            # only one version
bash build.sh python3.12 python3.14 # others
```

The `BlueEngine.cpp` and `main.py` need to be on the same folder (the `main.py` is integrated on the `.so` during the compilation; if not so, compile again). Each `.so` only works for it version of Python.

Manual comand for a version:

```bash
g++ -O2 -std=c++17 -shared -fPIC -fvisibility=hidden \
    $(python3.12 -m pybind11 --includes) $(pkg-config --cflags sdl2) \
    BlueEngine.cpp -o BlueEngine$(python3.12 -c "import sysconfig;print(sysconfig.get_config_var('EXT_SUFFIX'))") \
    $(pkg-config --libs sdl2) -lGL
```
(Changes may happen on the future)
