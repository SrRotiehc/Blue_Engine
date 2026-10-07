#Script that is embedded in BlueEngine. It has two jobs:
#1) Input processing (keys, mouse, closing the window, time between frames) using the raw SDL data that the C++ gives
#2) Turning the model (.obj) and textures (.png/.jpg) into arrays that the main engine can read
import numpy as np
from PIL import Image

#---------------- Connection with the C++ ----------------
#Before running this file the C++ gives it the BlueEngine module as "_raw". It only has raw data and the window:
#  _raw._start(w,h,title) / _raw._quit()   create / close the window
#  _raw._pollRaw()        events of this frame as a list of (kind, a, b):
#                         ("quit",0,0)  ("keydown",scancode,repeat)  ("keyup",scancode,0)  ("mouse",dx,dy)
#  _raw._keyHeld(code)    True while the key with this SDL scancode is held down
#  _raw._scancode(name)   SDL scancode of a key name ("w", "space", "left shift"...), 0 if the name is unknown
#  _raw._ticks()          milliseconds since SDL started
_raw=globals().get("_raw")

#Functions marked with @_export are copied into the BlueEngine module (BlueEngine.keyDown, BlueEngine.pollEvents...)
_exports={}
def _export(function):
    _exports[function.__name__]=function
    return function

#---------------- Input processing ----------------
class _State:
    def __init__(self):
        self.reset()

    def reset(self):
        self.quit=False       #the window was closed or stop() was called
        self.closed=False     #only the player closing the window (X button, Alt+F4...)
        self.pressed=set()    #scancodes pressed during this frame (once per key press)
        self.mouse=(0,0)      #mouse movement during this frame
        self.dt=0.0           #seconds between the last two pollEvents()
        self.last_ticks=0

_s=_State()
_scancodes={}                 #cache: key name -> scancode

def _code(name):
    code=_scancodes.get(name)
    if code is None:
        code=_raw._scancode(name)
        if code==0:
            raise ValueError("Unknown key name: "+str(name))
        _scancodes[name]=code
    return code

#Creates the window
@_export
def start(width=1600,height=800,title="Blue Engine"):
    _raw._start(width,height,title)
    _s.reset()
    _s.last_ticks=_raw._ticks()

#Closes the window. Safe to call more than once
@_export
def quit():
    _s.quit=True
    _raw._quit()

#Reads the events of this frame (call once per frame)
@_export
def pollEvents():
    events=_raw._pollRaw()
    pressed=set()
    dx=0
    dy=0
    for kind,a,b in events:
        if kind=="keydown":
            if b==0:                  #b is the repeat flag: ignores the auto-repeat of a held key
                pressed.add(a)
        elif kind=="mouse":
            dx+=a
            dy+=b
        elif kind=="quit":
            _s.quit=True
            _s.closed=True
    _s.pressed=pressed
    _s.mouse=(dx,dy)

    now=_raw._ticks()
    _s.dt=min((now-_s.last_ticks)/1000.0,0.1)   #the limit avoids a big jump after a slow frame (like loading a model)
    _s.last_ticks=now

#Key names are the SDL ones: "w", "space", "escape", "left shift"...
@_export
def keyDown(key):
    #True while the key is held down
    return _raw._keyHeld(_code(key))

@_export
def keyPressed(key):
    #True only in the frame where the key was pressed
    return _code(key) in _s.pressed

@_export
def mousePos():
    #Mouse movement (dx, dy) during this frame
    return _s.mouse

@_export
def windowClosed():
    #True when the player closed the window (X button, Alt+F4...)
    return _s.closed

@_export
def running():
    #True until the window is closed or stop()/quit() is called
    return not _s.quit

@_export
def stop():
    _s.quit=True

@_export
def deltaTime():
    #Seconds between the last two pollEvents()
    return _s.dt

#---------------- Model and texture loading ----------------
def load_obj(path):
    vertices=[]
    uvs=[]
    faces=[]
    face_uvs=[]

    #.obj indices start at 1; negative ones are relative to the end of the list
    def fix(index,count):
        return index-1 if index>0 else count+index

    with open(path,"r") as file:
        for line in file:
            parts=line.strip().split()
            if not parts:
                continue
            if parts[0]=="v":
                vertices.append([float(parts[1]),float(parts[2]),float(parts[3])])
            elif parts[0]=="vt":
                uvs.append([float(parts[1]),float(parts[2])])
            elif parts[0]=="f":
                face=[]
                face_uv=[]
                for part in parts[1:]:
                    idx=part.split("/")   #"v", "v/vt", "v/vt/vn" or "v//vn"
                    face.append(fix(int(idx[0]),len(vertices)))
                    if len(idx)>1 and idx[1]!="":
                        face_uv.append(fix(int(idx[1]),len(uvs)))
                    else:
                        face_uv.append(-1)   #this corner has no texture coordinate
                faces.append(face)
                face_uvs.append(face_uv)
    return (np.array(vertices,dtype=float),faces,
            np.array(uvs,dtype=float).reshape(-1,2),face_uvs)

def load_texture(path):
    #Returns the image as an array (height, width, 4) of bytes in RGBA order
    img=Image.open(path).convert("RGBA")
    return np.array(img,dtype=np.uint8)
