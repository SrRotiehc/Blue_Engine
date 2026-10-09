#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>
namespace py=pybind11;

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <iostream>
#include <string>
#include <tuple>
#include <array>
#include <set>
#include <memory>
#include <stdexcept>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <vector>

//main.py (loads the .obj and the textures) is copied into this .so when it is compiled, so it does not need to exist at run time.
//Linux (GCC/Clang) only. main.py must be in the folder where the compiler runs; if it changes, compile again
__asm__(
    ".section .rodata\n"
    ".balign 16\n"
    ".global blueengine_loader_start\n"
    ".hidden blueengine_loader_start\n"
    "blueengine_loader_start:\n"
    ".incbin \"main.py\"\n"
    "blueengine_loader_end:\n"
    ".byte 0\n"
    ".balign 4\n"
    ".global blueengine_loader_size\n"
    ".hidden blueengine_loader_size\n"
    "blueengine_loader_size:\n"
    ".long blueengine_loader_end - blueengine_loader_start\n"
    ".previous\n");
extern "C" const char blueengine_loader_start[];
extern "C" const unsigned int blueengine_loader_size;

//Names of the functions inside main.py
const char* PY_LOAD_OBJ="load_obj";
const char* PY_LOAD_TEXTURE="load_texture";

//Runs the embedded main.py when BlueEngine is imported. main.py receives this module as "_raw" (the raw SDL data)
//and the functions it marks with @_export are copied into this module (BlueEngine.pollEvents, BlueEngine.keyDown...)
void loadEmbeddedMain(py::module_ raw){
    py::module_ sys=py::module_::import("sys");
    py::module_ builtins=py::module_::import("builtins");
    py::module_ mod=py::module_::import("types").attr("ModuleType")("_BlueEngine_loader");
    py::dict ns=mod.attr("__dict__");
    ns["_raw"]=raw;

    std::string code(blueengine_loader_start,blueengine_loader_size);
    py::object compiled=builtins.attr("compile")(code,"main.py (inside BlueEngine)","exec"); //so errors show a readable file name
    builtins.attr("exec")(compiled,ns);

    py::dict exports=mod.attr("_exports");
    for (auto item: exports) py::setattr(raw,item.first,item.second);

    py::dict modules=sys.attr("modules");
    modules["_BlueEngine_loader"]=mod; //python keeps main.py alive here
}

//The embedded main.py (loadModel uses its load_obj and load_texture)
py::module_ loaderModule(){
    py::dict modules=py::module_::import("sys").attr("modules");
    return modules["_BlueEngine_loader"].cast<py::module_>();
}

//Model data loaded from main.py
struct Model_set{
    std::vector<float> vertices; //x,y,z, x,y,z, ...
    std::vector<std::vector<int>> faces; //vertex indices of each face
    std::vector<float> uvs; //u,v, u,v, ...
    std::vector<std::vector<int>> faceUV; //uv index of each corner of each face (-1 = none)
    GLuint texture=0; //OpenGL texture id (0 = no texture)
    int texW=0, texH=0; //texture size in pixels (used to give the flat plane the same proportion as the image)
    bool transparent=false; //true = the alpha channel of the texture is used (transparent pixels), false = alpha is ignored
    bool blank=false;       //plane created with neither .obj nor texture: with transparent=true it is not drawn at all
    double pos[3]={0.0,0.0,0.0};
    float scale[3]={1.0f,1.0f,1.0f};   //x, y, z
    float rot[3]={0.0f,0.0f,0.0f};     //x, y, z (degrees)
};

//In case these ever changes
const double pi_val=3.141592653589793;
const double mul_color=0.003921569;

//Engine state (everything the python script controls through the "BlueEngine" module)
int w_x=1600;
int w_y=800;
SDL_Window* g_window=nullptr;
SDL_GLContext g_context=nullptr;
bool g_glAlive=false;                  //true while the OpenGL context exists
float g_shade[3]={1.0f,1.0f,1.0f};     //global shade color
bool g_wireframe=false;

//Camera that the python script moves
struct Camera{
    std::array<double,3> pos={0.0,0.0,5.0}; //x, y, z (starts away from the origin)
    float yaw=0.0f;    //horizontal rotation (degrees)
    float pitch=0.0f;  //vertical rotation (degrees)
    float fov=60.0f;
    bool dirty=true;   //the projection must be rebuilt (fov changed)

    void setPitch(float p){ pitch=p>89.0f?89.0f:(p<-89.0f?-89.0f:p); }
    void rotate(float dyaw,float dpitch){ yaw+=dyaw; setPitch(pitch+dpitch); }
};
Camera g_camera;

//Asks main.py for the model and fills the arrays
bool loadModel(const std::string& path,Model_set&m){
    try{
        py::module_ loader=loaderModule();
        py::tuple res=loader.attr(PY_LOAD_OBJ)(path);

        //res = (vertices, faces, uvs, face_uvs)
        auto arr=res[0].cast<py::array_t<double,py::array::c_style|py::array::forcecast>>();
        m.faces=res[1].cast<std::vector<std::vector<int>>>();
        auto uv=res[2].cast<py::array_t<double,py::array::c_style|py::array::forcecast>>();
        m.faceUV=res[3].cast<std::vector<std::vector<int>>>();

        const double* p=arr.data();
        for (py::ssize_t i=0; i<arr.size(); i++) m.vertices.push_back((float)p[i]);
        const double* q=uv.data();
        for (py::ssize_t i=0; i<uv.size(); i++) m.uvs.push_back((float)q[i]);
        return true;
    }catch(const py::error_already_set& e){
        std::cout<<"Python error: "<<e.what()<<"\n";
        return false;
    }
}

//Asks main.py for the image and sends it to the GPU (needs the OpenGL context to exist)
bool loadTexture(const std::string& path,Model_set&m){
    try{
        py::module_ loader=loaderModule();
        auto img=loader.attr(PY_LOAD_TEXTURE)(path).cast<py::array_t<uint8_t,py::array::c_style|py::array::forcecast>>();
        int h=(int)img.shape(0);
        int w=(int)img.shape(1);
        m.texW=w; m.texH=h;

        glGenTextures(1,&m.texture);
        glBindTexture(GL_TEXTURE_2D,m.texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT,1);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR); //GL_NEAREST for a pixelated look
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,img.data());
        return true;
    }catch(const py::error_already_set& e){
        std::cout<<"Python error: "<<e.what()<<"\n";
        return false;
    }
}

//Builds a flat 2D model (like a sheet of paper) for a texture, used when loadModel gets a texture but no .obj
//It lies on the XY plane, centered on the origin and facing +Z (towards a camera that looks down -Z).
//Its height is 1 unit and its width keeps the proportion of the image, so the picture is never stretched.
//There is no face culling in drawModel, so both sides of the paper are visible.
//pos, scale and rotation work exactly like in any other model
void makePlane(Model_set&m){
    float aspect=(m.texW>0 && m.texH>0)?(float)m.texW/(float)m.texH:1.0f;
    float hw=aspect*0.5f, hh=0.5f;

    m.vertices={-hw,-hh,0.0f,   hw,-hh,0.0f,   hw,hh,0.0f,   -hw,hh,0.0f};
    m.faces={{0,1,2,3}};
    m.uvs={0.0f,0.0f,  1.0f,0.0f,  1.0f,1.0f,  0.0f,1.0f}; //same convention as .obj (v=0 at the bottom)
    m.faceUV={{0,1,2,3}};
}

//Draws every face of the model, splitting faces with 4+ corners into triangles
//Order for each vertex: scale -> rotate (X, then Y, then Z) -> position relative to the camera
void drawModel(Model_set& m){

    //A plane made without model and without texture has nothing to show through alpha, so
    //transparent=true makes the whole plane invisible (and it does not hide other models)
    if (m.blank && m.transparent) return;

    //Makes all the big calculations of vertices in doubles and then convert to float
    float cx=(float)(m.pos[0]-g_camera.pos[0]);
    float cy=(float)(m.pos[1]-g_camera.pos[1]);
    float cz=(float)(m.pos[2]-g_camera.pos[2]);

    //Rotation angles (degrees -> radians) and their sin/cos, computed once instead of once per vertex
    float rx=(float)(m.rot[0]*pi_val/180.0);
    float ry=(float)(m.rot[1]*pi_val/180.0);
    float rz=(float)(m.rot[2]*pi_val/180.0);
    float cosx=cosf(rx), sinx=sinf(rx);
    float cosy=cosf(ry), siny=sinf(ry);
    float cosz=cosf(rz), sinz=sinf(rz);

    //Only textures if there is an image and every face has its uv list
    bool useTex=m.texture!=0 && !m.uvs.empty() && m.faceUV.size()==m.faces.size();
    if (useTex){
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D,m.texture);
    }

    //Transparency (only for textured models): alpha blends the pixel with what is behind it, and fully transparent
    //pixels are discarded so they do not hide other models through the depth buffer.
    //For several semi-transparent models, draw them after the opaque ones and from the farthest to the nearest
    bool useAlpha=m.transparent && useTex;
    if (useAlpha){
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER,0.01f);
    }

    glBegin(GL_TRIANGLES);
        for (size_t f=0; f<m.faces.size(); f++){
            const auto& face=m.faces[f];
            for (size_t i=1; i+1<face.size(); i++){
                size_t corners[3]={0,i,i+1}; //fan: corner 0 + each consecutive pair
                for (size_t c: corners){
                    if (useTex){
                        int t=m.faceUV[f][c];
                        if (t>=0){
                            const float* uv=&m.uvs[t*2];
                            glTexCoord2f(uv[0],1.0f-uv[1]); //.obj has v=0 at the bottom, images at the top
                        }
                    }
                    const float* v=&m.vertices[face[c]*3];

                    //Scale
                    float x=v[0]*m.scale[0];
                    float y=v[1]*m.scale[1];
                    float z=v[2]*m.scale[2];

                    // X
                    float x1=x;
                    float y1=y*cosx-z*sinx;
                    float z1=y*sinx+z*cosx;

                    // Y
                    float x2=x1*cosy+z1*siny;
                    float y2=y1;
                    float z2=-x1*siny+z1*cosy;

                    // Z
                    float x3=x2*cosz-y2*sinz;
                    float y3=x2*sinz+y2*cosz;
                    float z3=z2;

                    glVertex3f(x3+cx,y3+cy,z3+cz);
                }
            }
        }
    glEnd();

    //Puts the state back so the next model is not affected
    if (useAlpha){
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_BLEND);
    }
    glDisable(GL_TEXTURE_2D);
}

//Positive = multiplies, negative = divides (-2 means half the size), 0 = keeps the original size
float scaleFactor(float s){
    if (s==0.0f) return 1.0f;
    return s>0.0f?s:1.0f/(-s);
}

//A model that python holds. Its texture is freed when python releases it or when the engine quits
class Model{
public:
    Model_set data;

    Model()=default;
    Model(const Model&)=delete;
    Model& operator=(const Model&)=delete;
    ~Model(){ unload(); }

    void render(){ drawModel(data); }

    //Frees the texture from the GPU (only possible while the OpenGL context exists)
    void unload(){
        if (g_glAlive && data.texture!=0) glDeleteTextures(1,&data.texture);
        data.texture=0;
    }
};
std::vector<std::weak_ptr<Model>> g_models; //every model that was loaded, so quit() can free them

//Scale from python: one number (all directions) or three numbers (x,y,z)
void setScaleFrom(Model_set& d,const py::object& o){
    if (py::isinstance<py::sequence>(o)){
        auto v=o.cast<std::array<float,3>>();
        for (int i=0; i<3; i++) d.scale[i]=scaleFactor(v[i]);
    }else{
        float s=scaleFactor(o.cast<float>());
        d.scale[0]=s; d.scale[1]=s; d.scale[2]=s;
    }
}

void requireGL(){
    if (!g_glAlive) throw std::runtime_error("The engine is not running: call BlueEngine.start() first");
}

//Rebuilds the perspective projection (60 degrees fov by default)
void applyProjection(){
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    float aspect=(float)w_x/w_y, nearP=0.1f, farP=5000.0f;
    float top=nearP*tanf(g_camera.fov*pi_val/180.0f/2.0f);
    glFrustum(-top*aspect,top*aspect,-top,top,nearP,farP);
    glMatrixMode(GL_MODELVIEW);
    g_camera.dirty=false;
}

//Functions that python can call BlueEngine

//Creates the window and the OpenGL context
void engineInit(int width,int height,const std::string& title){
    if (g_glAlive) throw std::runtime_error("BlueEngine.start was already called");
    w_x=width; w_y=height;

    if (SDL_Init(SDL_INIT_VIDEO)!=0) throw std::runtime_error(std::string("SDL error: ")+SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,24); //Depth buffer for 3D

    g_window=SDL_CreateWindow(title.c_str(),SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,w_x,w_y,SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN);
    if (!g_window) throw std::runtime_error(std::string("Window error: ")+SDL_GetError());
    g_context=SDL_GL_CreateContext(g_window);
    if (!g_context) throw std::runtime_error(std::string("Context error: ")+SDL_GetError());
    SDL_GL_SetSwapInterval(1); //Vsync
    glViewport(0,0,w_x,w_y);
    glEnable(GL_DEPTH_TEST);

    g_glAlive=true;
    applyProjection();
}

//Frees the models and closes the window. Safe to call more than once
void engineQuit(){
    for (auto& w: g_models) if (auto m=w.lock()) m->unload();
    g_models.clear();
    g_glAlive=false;
    if (g_context){ SDL_GL_DeleteContext(g_context); g_context=nullptr; }
    if (g_window){ SDL_DestroyWindow(g_window); g_window=nullptr; SDL_Quit(); }
}

//Raw input for main.py: this only reads SDL. The processing (pressed keys, mouse movement,
//closing the window, time between frames) is done in main.py

//Events of this frame as tuples (kind, a, b):
//("quit",0,0)  ("keydown",scancode,repeat)  ("keyup",scancode,0)  ("mouse",xrel,yrel)
py::list engineRawEvents(){
    requireGL();
    py::list out;
    SDL_Event event;
    while (SDL_PollEvent(&event)){
        if (event.type==SDL_QUIT) out.append(py::make_tuple("quit",0,0));
        else if (event.type==SDL_KEYDOWN) out.append(py::make_tuple("keydown",(int)event.key.keysym.scancode,(int)event.key.repeat));
        else if (event.type==SDL_KEYUP) out.append(py::make_tuple("keyup",(int)event.key.keysym.scancode,0));
        else if (event.type==SDL_MOUSEMOTION) out.append(py::make_tuple("mouse",(int)event.motion.xrel,(int)event.motion.yrel));
    }
    return out;
}

//True while the key with this SDL scancode is held down
bool engineKeyHeld(int scancode){
    requireGL();
    if (scancode<=0 || scancode>=SDL_NUM_SCANCODES) return false;
    return SDL_GetKeyboardState(NULL)[scancode]!=0;
}

//SDL scancode of a key name ("w", "space", "escape", "left shift"...), 0 if the name is unknown
int engineScancode(const std::string& name){ return (int)SDL_GetScancodeFromName(name.c_str()); }

void engineSetMouseCapture(bool on){ requireGL(); SDL_SetRelativeMouseMode(on?SDL_TRUE:SDL_FALSE); }
bool engineMouseCaptured(){ requireGL(); return SDL_GetRelativeMouseMode()==SDL_TRUE; }

//Clears the screen with a color (0-255) and places the camera. Call at the start of the drawing
void engineClear(std::array<double,3> color){
    requireGL();
    if (g_camera.dirty) applyProjection();
    glClearColor(color[0]*mul_color,color[1]*mul_color,color[2]*mul_color,1.0f);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

    //engine
    //Camera: rotates the world around the camera (drawModel already subtracts its position)
    glLoadIdentity();
    glRotatef(g_camera.pitch,1.0f,0.0f,0.0f);
    glRotatef(g_camera.yaw,0.0f,1.0f,0.0f);

    glColor3f(g_shade[0],g_shade[1],g_shade[2]);
    glPolygonMode(GL_FRONT_AND_BACK,g_wireframe?GL_LINE:GL_FILL);
}

//Shows what was drawn
void engineFlip(){
    requireGL();
    glFlush();
    SDL_GL_SwapWindow(g_window);
}

//Loads a model (and its texture, if any) to memory
//If obj is empty but texture is not, it creates a flat plane (like a sheet of paper) with that texture
//If both are empty, it creates a blank 1x1 plane (shade color). With transparent=true that plane is invisible
//transparent=true makes the alpha channel of the texture count (PNG with transparent areas); false ignores it
std::shared_ptr<Model> engineLoadModel(const std::string& obj,const std::string& texture,
                                       std::array<double,3> pos,const py::object& scale,std::array<float,3> rotation,bool transparent){
    requireGL();
    auto md=std::make_shared<Model>();
    for (int i=0; i<3; i++){ md->data.pos[i]=pos[i]; md->data.rot[i]=rotation[i]; }
    setScaleFrom(md->data,scale);
    md->data.transparent=transparent;

    if (obj.empty()){
        if (texture.empty()){
            //Blank plane: 1x1 square with no texture, drawn with the shade color.
            //With transparent=true it is invisible (it still exists: it can be moved, scaled, rotated and turned visible later)
            md->data.blank=true;
        }else{
            //Plane mode: the texture is the whole point, so failing to load it is an error
            if (!loadTexture(texture,md->data)) throw std::runtime_error("Could not load the texture: "+texture);
        }
        makePlane(md->data);
    }else{
        if (!loadModel(obj,md->data)) throw std::runtime_error("Could not load the model: "+obj);
        if (!texture.empty() && !loadTexture(texture,md->data)) std::cout<<"No texture loaded, drawing without it\n";
    }

    g_models.push_back(md);
    return md;
}

PYBIND11_MODULE(BlueEngine,m){
    m.doc()="BlueEngine: window, input, camera and models controlled from python";
    py::module_::import("atexit").attr("register")(py::cpp_function(&engineQuit));

    //Window and frame (start, quit, running, stop, windowClosed, pollEvents and deltaTime are written in main.py and use these)
    m.def("_start",&engineInit,py::arg("width"),py::arg("height"),py::arg("title"));
    m.def("_quit",&engineQuit);
    m.def("windowTitle",[](const std::string& t){ requireGL(); SDL_SetWindowTitle(g_window,t.c_str()); });
    m.def("clear",&engineClear,py::arg("color")=std::make_tuple(0.0,0.0,0.0));
    m.def("flip",&engineFlip);

    //Render settings
    m.def("setShade",[](std::array<double,3> c){ for (int i=0; i<3; i++) g_shade[i]=(float)(c[i]*mul_color); },py::arg("color"));
    m.def("setWireframe",[](bool on){ g_wireframe=on; },py::arg("on"));

    //Input: raw SDL data for main.py (keyDown, keyPressed, mousePos... are written in main.py)
    m.def("_pollRaw",&engineRawEvents);
    m.def("_keyHeld",&engineKeyHeld,py::arg("scancode"));
    m.def("_scancode",&engineScancode,py::arg("name"));
    m.def("_ticks",[](){ return (unsigned int)SDL_GetTicks(); });
    m.def("setMouseCap",&engineSetMouseCapture,py::arg("on"));
    m.def("mouseCap",&engineMouseCaptured);

    //Camera
    //Camera: called with values it sets them (camera.pos(0,0,5)), called without values it returns the current ones (camera.pos())
    py::class_<Camera>(m,"Camera")
        .def("pos",[](const Camera& c){ return std::make_tuple(c.pos[0],c.pos[1],c.pos[2]); })
        .def("pos",[](Camera& c,double x,double y,double z){ c.pos={x,y,z}; },py::arg("x"),py::arg("y"),py::arg("z"))
        .def("pos",[](Camera& c,std::array<double,3> v){ c.pos=v; },py::arg("xyz"))
        .def("yaw",[](const Camera& c){ return c.yaw; })
        .def("yaw",[](Camera& c,float degrees){ c.yaw=degrees; },py::arg("degrees"))
        .def("pitch",[](const Camera& c){ return c.pitch; })
        .def("pitch",[](Camera& c,float degrees){ c.setPitch(degrees); },py::arg("degrees"))
        .def("fov",[](const Camera& c){ return c.fov; })
        .def("fov",[](Camera& c,float degrees){ c.fov=degrees; c.dirty=true; },py::arg("degrees"))
        .def("rotate",&Camera::rotate,py::arg("yaw")=0.0f,py::arg("pitch")=0.0f);
    m.attr("camera")=py::cast(&g_camera,py::return_value_policy::reference);

    //Models
    py::class_<Model,std::shared_ptr<Model>>(m,"Model")
        //Called with values they set them (model.pos(0,-1,-5)), called without values they return the current ones (model.pos())
        .def("pos",[](const Model& md){ return std::make_tuple(md.data.pos[0],md.data.pos[1],md.data.pos[2]); })
        .def("pos",[](Model& md,double x,double y,double z){ md.data.pos[0]=x; md.data.pos[1]=y; md.data.pos[2]=z; },
             py::arg("x"),py::arg("y"),py::arg("z"))
        .def("pos",[](Model& md,std::array<double,3> v){ for (int i=0; i<3; i++) md.data.pos[i]=v[i]; },py::arg("xyz"))
        .def("scale",[](const Model& md){ return std::make_tuple(md.data.scale[0],md.data.scale[1],md.data.scale[2]); })
        .def("scale",[](Model& md,float x,float y,float z){
                 md.data.scale[0]=scaleFactor(x); md.data.scale[1]=scaleFactor(y); md.data.scale[2]=scaleFactor(z); },
             py::arg("x"),py::arg("y"),py::arg("z"))
        .def("scale",[](Model& md,const py::object& o){ setScaleFrom(md.data,o); },py::arg("value")) //one number or (x,y,z)
        .def("rotation",[](const Model& md){ return std::make_tuple(md.data.rot[0],md.data.rot[1],md.data.rot[2]); })
        .def("rotation",[](Model& md,float x,float y,float z){ md.data.rot[0]=x; md.data.rot[1]=y; md.data.rot[2]=z; },
             py::arg("x"),py::arg("y"),py::arg("z"))
        .def("rotation",[](Model& md,std::array<float,3> v){ for (int i=0; i<3; i++) md.data.rot[i]=v[i]; },py::arg("xyz"))
        .def("move",[](Model& md,double x,double y,double z){ md.data.pos[0]+=x; md.data.pos[1]+=y; md.data.pos[2]+=z; },
             py::arg("x")=0.0,py::arg("y")=0.0,py::arg("z")=0.0)
        .def("rotate",[](Model& md,float x,float y,float z){ md.data.rot[0]+=x; md.data.rot[1]+=y; md.data.rot[2]+=z; },
             py::arg("x")=0.0f,py::arg("y")=0.0f,py::arg("z")=0.0f)
        //Called with a value it sets the transparency (model.transparent(True)), without a value it returns it
        .def("transparent",[](const Model& md){ return md.data.transparent; })
        .def("transparent",[](Model& md,bool on){ md.data.transparent=on; },py::arg("on"))
        .def("render",&Model::render)
        .def("unload",&Model::unload);

    m.def("loadModel",&engineLoadModel,
          py::arg("obj")="",py::arg("texture")="",
          py::arg("pos")=std::make_tuple(0.0,0.0,0.0),
          py::arg("scale")=1.0,
          py::arg("rotation")=std::make_tuple(0.0,0.0,0.0),
          py::arg("transparent")=false);

    //Runs main.py last, so everything it uses from this module already exists
    loadEmbeddedMain(m);
}
