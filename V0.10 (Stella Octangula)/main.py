#Script that is embedded in BlueEngine. It has two jobs:
#1) Input processing (keys, mouse, closing the window, time between frames) using the raw SDL data that the C++ gives
#2) Turning the model (.obj) and textures (.png/.jpg) into arrays that the main engine can send to the GPU as they are
#   (a model becomes a vertex array for the VBO and an index array for the EBO; see load_obj)
#3) Listing the GPUs of the computer and choosing which one the engine uses (setGPU)
import operator
import os
import numpy as np
from PIL import Image

#---------------- Connection with the C++ ----------------
#Before running this file the C++ gives it the BlueEngine module as "_raw". It only has raw data and the window:
#  _raw._start(w,h,title) / _raw._quit()   create / close the window
#  _raw._pollRaw()        events of this frame: keydown/keyup, mouse movement, mouse button down and quit
#  _raw._keyHeld(code)    True while the key with this SDL scancode is held down
#  _raw._scancode(name)   SDL scancode of a key name ("w", "space", "left shift"...), 0 if the name is unknown
#  _raw._mouseHeld(btn)   True while an SDL mouse button is held down
#  _raw._mousePosXY()     absolute mouse position (x, y) in window coordinates
#  _raw._ticks()          milliseconds (with decimals) from an arbitrary start: only the difference between two calls matters
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
        self.mouse_pressed=set()  #mouse buttons pressed during this frame
        self.dt=0.0           #seconds between the last two pollEvents()
        self.last_ticks=0
        self.started=False    #the window exists (start() was called and quit() was not)

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
    _s.started=True

#Closes the window. Safe to call more than once
@_export
def quit():
    _s.quit=True
    _s.started=False
    _raw._quit()

#Reads the events of this frame (call once per frame)
@_export
def pollEvents():
    events=_raw._pollRaw()
    pressed=set()
    mouse_pressed=set()
    dx=0
    dy=0
    for kind,a,b in events:
        if kind=="keydown":
            if b==0:                  #b is the repeat flag: ignores the auto-repeat of a held key
                pressed.add(a)
        elif kind=="mouse":
            dx+=a
            dy+=b
        elif kind=="mousebuttondown":
            mouse_pressed.add(a)
        elif kind=="quit":
            _s.quit=True
            _s.closed=True
    _s.pressed=pressed
    _s.mouse_pressed=mouse_pressed
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

_MOUSE_BUTTONS={"left":1,"middle":2,"right":3,"x1":4,"x2":5}

def _mouse_button(button):
    if isinstance(button,str):
        name=button.strip().lower().replace(" button", "")
        if name in _MOUSE_BUTTONS:
            return _MOUSE_BUTTONS[name]
        raise ValueError("Unknown mouse button: "+str(button))
    if isinstance(button,bool):
        raise TypeError("Mouse button must be a name or an integer from 1 to 5")
    try:
        button=operator.index(button)
    except TypeError:
        raise TypeError("Mouse button must be a name or an integer from 1 to 5") from None
    if not 1<=button<=5:
        raise ValueError("Mouse button must be from 1 to 5")
    return button

@_export
def mouseBTDown(button):
    #True while a mouse button is held down; 1=left, 2=middle, 3=right, 4/5=side buttons
    return _raw._mouseHeld(_mouse_button(button))

@_export
def mouseBTPressed(button):
    #True only in the frame where a mouse button was pressed
    return _mouse_button(button) in _s.mouse_pressed

@_export
def mousePosXY():
    #Absolute mouse position (x, y) in window coordinates
    return _raw._mousePosXY()

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

#---------------- GPU selection ----------------
#The graphics drivers read some environment variables when the OpenGL context is created to decide which GPU renders
#(PRIME render offload), so setGPU() sets them and has to be called BEFORE start().
#The GPUs are the PCI display controllers that have a driver loaded (the ones that can render). The system default
#one (the GPU used at boot) comes first, so number 0 is the default GPU on most computers
_PCI_DEVICES="/sys/bus/pci/devices"
_PCI_IDS_FILES=("/usr/share/hwdata/pci.ids","/usr/share/misc/pci.ids","/usr/share/pci.ids")   #names of the PCI vendors/devices
_EGL_NVIDIA="/usr/share/glvnd/egl_vendor.d/10_nvidia.json"
_EGL_MESA="/usr/share/glvnd/egl_vendor.d/50_mesa.json"
_gpu_env_backup={}   #variable -> value it had before setGPU changed it (None = it did not exist)

def _read_text(path):
    try:
        with open(path,"r") as file:
            return file.read().strip()
    except OSError:
        return None

#(vendor name, device name) written in the system's pci.ids, None for what is not found. Ids are 4 lowercase hex digits
def _pci_names(vendor,device):
    for path in _PCI_IDS_FILES:
        try:
            file=open(path,"r",encoding="utf-8",errors="replace")
        except OSError:
            continue
        with file:
            vendor_name=None
            for line in file:
                if not line.strip() or line[0]=="#":
                    continue
                if line[0]!="\t":                                 #a vendor line: "10de  NVIDIA Corporation"
                    if vendor_name is not None:
                        break                                     #the vendor block ended without the device
                    if line.startswith(vendor+"  "):
                        vendor_name=line[6:].strip()
                elif vendor_name is not None and line[1]!="\t":   #a device line: "\t2504  GA106 [GeForce RTX 3060...]"
                    if line[1:5]==device:
                        return vendor_name,line[7:].strip()
            return vendor_name,None
    return None,None

#Readable name like "NVIDIA GeForce RTX 3060" (without pci.ids it falls back to the ids)
def _gpu_name(vendor,device):
    vendor_name,device_name=_pci_names(vendor,device)
    short={"10de":"NVIDIA","1002":"AMD","1022":"AMD","8086":"Intel"}.get(vendor,vendor_name)
    if device_name and device_name.endswith("]") and "[" in device_name:
        device_name=device_name[device_name.rindex("[")+1:-1]   #"GA106 [GeForce RTX 3060]" -> "GeForce RTX 3060"
    if short is None:
        if device_name is None:
            return "GPU %s:%s"%(vendor,device)
        short=vendor
    if device_name is None:
        return "%s GPU (device %s)"%(short,device)
    if device_name.lower().startswith(short.lower()):
        return device_name
    return "%s %s"%(short,device_name)

#The GPUs of the computer, the default one first and then by PCI address.
#Each one is a dict: slot ("0000:01:00.0"), driver ("nvidia", "amdgpu", "i915"...), name and default (True/False)
def _list_gpus():
    gpus=[]
    try:
        slots=sorted(os.listdir(_PCI_DEVICES))
    except OSError:
        return gpus                                          #no PCI information (not Linux, or a computer without PCI)
    for slot in slots:
        folder=os.path.join(_PCI_DEVICES,slot)
        pci_class=_read_text(os.path.join(folder,"class"))
        if not pci_class or not pci_class.startswith("0x03"):   #class 03 = display controller (VGA, 3D...)
            continue
        try:
            driver=os.path.basename(os.readlink(os.path.join(folder,"driver")))
        except OSError:
            continue                                         #without a driver the GPU cannot render
        if driver in ("vfio-pci","pci-stub"):
            continue                                         #reserved for a virtual machine
        vendor=(_read_text(os.path.join(folder,"vendor")) or "0x0000")[2:].lower()
        device=(_read_text(os.path.join(folder,"device")) or "0x0000")[2:].lower()
        gpus.append({"slot":slot,"driver":driver,"name":_gpu_name(vendor,device),
                     "default":_read_text(os.path.join(folder,"boot_vga"))=="1"})
    gpus.sort(key=lambda g:(not g["default"],g["slot"]))
    return gpus

#Puts the environment variables of one GPU (or the original ones when gpu is None)
def _apply_gpu_env(gpu,gpus):
    for name,old in _gpu_env_backup.items():                 #first undoes the previous choice
        if old is None:
            os.environ.pop(name,None)
        else:
            os.environ[name]=old
    _gpu_env_backup.clear()
    if gpu is None:
        return

    new={}
    if gpu["driver"]=="nvidia":
        #Proprietary NVIDIA driver (PRIME render offload)
        new["__NV_PRIME_RENDER_OFFLOAD"]="1"
        new["__GLX_VENDOR_LIBRARY_NAME"]="nvidia"
        new["__VK_LAYER_NV_optimus"]="NVIDIA_only"
        if os.path.exists(_EGL_NVIDIA):
            new["__EGL_VENDOR_LIBRARY_FILENAMES"]=_EGL_NVIDIA     #for EGL (Wayland)
    else:
        #Open source Mesa drivers (AMD, Intel, nouveau...): the GPU is chosen by its PCI address, "pci-0000_01_00_0"
        new["DRI_PRIME"]="pci-"+gpu["slot"].replace(":","_").replace(".","_")
        if any(g["default"] and g["driver"]=="nvidia" for g in gpus):
            #The screen belongs to the proprietary NVIDIA driver, so Mesa has to be asked for explicitly
            new["__GLX_VENDOR_LIBRARY_NAME"]="mesa"
            if os.path.exists(_EGL_MESA):
                new["__EGL_VENDOR_LIBRARY_FILENAMES"]=_EGL_MESA
    for name,value in new.items():
        _gpu_env_backup[name]=os.environ.get(name)
        os.environ[name]=value

#What setGPU returns: a list with the description of each GPU ("NVIDIA GeForce RTX 3060 (nvidia, 0000:01:00.0)").
#print() shows it numbered, with the default and the chosen GPU marked
class _GPUList(list):
    def __init__(self,gpus,selected=None):
        super().__init__("%s (%s, %s)"%(g["name"],g["driver"],g["slot"]) for g in gpus)
        self._tags=[(["default"] if g["default"] else [])+(["selected"] if i==selected else []) for i,g in enumerate(gpus)]

    def __str__(self):
        if not self:
            return "Available GPUs: none found (the system default is used)"
        lines=["Available GPUs:"]
        for i,text in enumerate(self):
            tags=self._tags[i] if i<len(self._tags) else []
            lines.append("  %d: %s%s"%(i,text,("  ["+", ".join(tags)+"]") if tags else ""))
        return "\n".join(lines)

    __repr__=__str__

_gpu_choice=None   #number chosen with setGPU (None = the system default)

#setGPU()     -> uses the GPU that the system picks by default
#setGPU(n)    -> uses the GPU number n (see the list); a number that is not a GPU raises ValueError
#It always returns the list of available GPUs, so print(setGPU()) shows which ones exist.
#It has to be called before start(): once the window exists its GPU cannot change (then only the listing works)
#After start(), gpuName() tells which GPU is really drawing
@_export
def setGPU(index=None):
    global _gpu_choice
    gpus=_list_gpus()
    if _s.started:
        if index is not None:
            raise RuntimeError("setGPU must be called before start(): the window already exists and its GPU cannot change")
        return _GPUList(gpus,_gpu_choice)

    if index is not None:
        if isinstance(index,bool):
            raise TypeError("setGPU needs the number of a GPU (an integer), or no value for the system default")
        try:
            index=operator.index(index)
        except TypeError:
            raise TypeError("setGPU needs the number of a GPU (an integer), or no value for the system default") from None
        if not 0<=index<len(gpus):
            if gpus:
                raise ValueError("GPU %d does not exist (valid numbers: 0 to %d). print(setGPU()) lists them"%(index,len(gpus)-1))
            raise ValueError("GPU %d does not exist: no GPU was found on this computer"%index)

    _gpu_choice=index
    _apply_gpu_env(gpus[index] if index is not None else None,gpus)
    return _GPUList(gpus,index)

#---------------- Model and texture loading ----------------
#Reads a .obj and returns (vertex_data, indices, has_uv), ready to be copied to the GPU:
#  vertex_data  float32 array, 5 values per vertex: x, y, z, u, v   (goes to the VBO)
#  indices      uint32 array, 3 values per triangle                  (goes to the EBO)
#  has_uv       True if at least one corner of the model has a texture coordinate
#A .obj corner is a pair (position, texture coordinate), and the GPU needs one vertex for each different pair,
#so a corner that was already seen is reused instead of being repeated. Faces with 4 or more corners are
#split into triangles here (a fan: corner 0 + each consecutive pair). A corner without texture coordinate gets u=v=0.
#The v of the texture coordinates stays as in the .obj (0 at the bottom); the vertex shader flips it for the image
def load_obj(path):
    positions=[]     #(x,y,z) of every "v" line
    uvs=[]           #(u,v) of every "vt" line
    vertex_data=[]   #x,y,z,u,v of every GPU vertex, one after the other
    indices=[]
    unique={}        #(position index, uv index) -> index of the GPU vertex
    has_uv=False

    #.obj indices start at 1; negative ones are relative to the end of the list
    def fix(index,count):
        return index-1 if index>0 else count+index

    with open(path,"r") as file:
        for number,line in enumerate(file,1):
            parts=line.strip().split()
            if not parts:
                continue
            if parts[0]=="v":
                positions.append((float(parts[1]),float(parts[2]),float(parts[3])))
            elif parts[0]=="vt":
                uvs.append((float(parts[1]),float(parts[2])))
            elif parts[0]=="f":
                corners=[]
                for part in parts[1:]:
                    idx=part.split("/")   #"v", "v/vt", "v/vt/vn" or "v//vn"
                    p=fix(int(idx[0]),len(positions))
                    if not 0<=p<len(positions):
                        raise ValueError("%s, line %d: vertex index %s does not exist"%(path,number,idx[0]))
                    t=-1                  #this corner has no texture coordinate
                    if len(idx)>1 and idx[1]!="":
                        t=fix(int(idx[1]),len(uvs))
                        if not 0<=t<len(uvs):
                            raise ValueError("%s, line %d: texture coordinate index %s does not exist"%(path,number,idx[1]))
                    key=(p,t)
                    n=unique.get(key)
                    if n is None:
                        n=len(unique)
                        unique[key]=n
                        u,v=uvs[t] if t>=0 else (0.0,0.0)
                        vertex_data.extend((positions[p][0],positions[p][1],positions[p][2],u,v))
                        if t>=0:
                            has_uv=True
                    corners.append(n)
                for i in range(1,len(corners)-1):
                    indices.extend((corners[0],corners[i],corners[i+1]))
    return (np.array(vertex_data,dtype=np.float32),
            np.array(indices,dtype=np.uint32),
            has_uv)

def load_texture(path):
    #Returns the image as an array (height, width, 4) of bytes in RGBA order
    img=Image.open(path).convert("RGBA")
    return _bleed_transparent_colors(np.array(img,dtype=np.uint8))

#Fully transparent pixels still store a color (white or black in most images, whatever the editor used), and the GPU
#mixes it into the edge of the drawing when it smooths the texture, leaving a border around the model with that color.
#This gives the transparent pixels that touch the image the color of their neighbors, so the mix is invisible.
#Only the color of pixels with alpha 0 changes (alpha stays 0, so with transparent=true nothing else is different)
def _bleed_transparent_colors(pixels,passes=2):
    alpha=pixels[:,:,3]
    known=alpha>0
    if known.all() or not known.any():
        return pixels                     #no transparency to fix (or nothing to copy the color from)
    height,width=alpha.shape
    filled=np.zeros_like(known)           #transparent pixels that already got a color
    offsets=[(dy,dx) for dy in (-1,0,1) for dx in (-1,0,1) if (dy,dx)!=(0,0)]
    for _ in range(passes):
        have=known|filled
        grown=have.copy()                 #grows the pixels with color by 1 in every direction (8 neighbors)
        grown[1:,:]|=have[:-1,:]
        grown[:-1,:]|=have[1:,:]
        rows=grown.copy()
        rows[:,1:]|=grown[:,:-1]
        rows[:,:-1]|=grown[:,1:]
        ys,xs=np.nonzero(rows&~have)      #pixels without color that touch one with color
        if len(ys)==0:
            break
        color_sum=np.zeros((len(ys),3),dtype=np.float32)
        weight_sum=np.zeros(len(ys),dtype=np.float32)
        for dy,dx in offsets:
            y2=ys+dy
            x2=xs+dx
            inside=(y2>=0)&(y2<height)&(x2>=0)&(x2<width)
            y2=np.clip(y2,0,height-1)
            x2=np.clip(x2,0,width-1)
            #opaque neighbors weigh more; a pixel filled in the previous pass counts little
            weight=np.where(known[y2,x2],alpha[y2,x2].astype(np.float32),np.where(filled[y2,x2],1.0,0.0))
            weight=np.where(inside,weight,0.0).astype(np.float32)
            color_sum+=pixels[y2,x2,:3]*weight[:,None]
            weight_sum+=weight
        pixels[ys,xs,:3]=np.rint(color_sum/weight_sum[:,None]).astype(np.uint8)
        filled[ys,xs]=True
    return pixels
