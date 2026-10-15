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
#include <algorithm>
#include <chrono>
#include <optional>
#include <sstream>
#include <thread>

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
//and the functions it marks with @_export are copied into this module (BlueEngine.flip, BlueEngine.keyDown...)
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

//---------------- How a model reaches the screen (GPU pipeline) ----------------
//Load time, once per model (the CPU only copies data):
//  main.py (or makePlane) gives the vertices (x,y,z,u,v) and the triangle indices -> they are copied to the GPU:
//    VBO = the vertices, EBO = the triangle indices, VAO = remembers how both are read
//Every frame, for each model.render():
//  CPU: builds ONE matrix (projection * camera * model position/rotation/scale) and sends it as a uniform
//  GPU, vertex shader:   position = matrix * vertex          (this used to be a loop over every vertex on the CPU)
//  GPU, fragment shader: color = shade color * texture, drawing the transparent pixels in a separate pass when transparent=true
//The vertex and fragment shader together are the "pipeline" (struct Pipeline below)

//OpenGL functions that SDL_opengl.h only declares as types (buffers, VAOs and shaders are OpenGL 2.0/3.0).
//They are asked to the driver with SDL_GL_GetProcAddress once the context exists and are called like gl.GenBuffers(...)
#define GL_FUNCTION_LIST(X) \
    X(PFNGLGENBUFFERSPROC,GenBuffers) \
    X(PFNGLBINDBUFFERPROC,BindBuffer) \
    X(PFNGLBUFFERDATAPROC,BufferData) \
    X(PFNGLDELETEBUFFERSPROC,DeleteBuffers) \
    X(PFNGLGENVERTEXARRAYSPROC,GenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC,BindVertexArray) \
    X(PFNGLDELETEVERTEXARRAYSPROC,DeleteVertexArrays) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC,EnableVertexAttribArray) \
    X(PFNGLVERTEXATTRIBPOINTERPROC,VertexAttribPointer) \
    X(PFNGLCREATESHADERPROC,CreateShader) \
    X(PFNGLSHADERSOURCEPROC,ShaderSource) \
    X(PFNGLCOMPILESHADERPROC,CompileShader) \
    X(PFNGLGETSHADERIVPROC,GetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC,GetShaderInfoLog) \
    X(PFNGLDELETESHADERPROC,DeleteShader) \
    X(PFNGLCREATEPROGRAMPROC,CreateProgram) \
    X(PFNGLATTACHSHADERPROC,AttachShader) \
    X(PFNGLLINKPROGRAMPROC,LinkProgram) \
    X(PFNGLGETPROGRAMIVPROC,GetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC,GetProgramInfoLog) \
    X(PFNGLDELETEPROGRAMPROC,DeleteProgram) \
    X(PFNGLUSEPROGRAMPROC,UseProgram) \
    X(PFNGLGETUNIFORMLOCATIONPROC,GetUniformLocation) \
    X(PFNGLUNIFORM1IPROC,Uniform1i) \
    X(PFNGLUNIFORM1FPROC,Uniform1f) \
    X(PFNGLUNIFORM2FPROC,Uniform2f) \
    X(PFNGLUNIFORM3FPROC,Uniform3f) \
    X(PFNGLUNIFORMMATRIX4FVPROC,UniformMatrix4fv)

struct GLFunctions{
#define X(type,name) type name=nullptr;
    GL_FUNCTION_LIST(X)
#undef X
};
GLFunctions gl;

void loadGLFunctions(){
    std::string missing;
#define X(type,name) gl.name=reinterpret_cast<type>(SDL_GL_GetProcAddress("gl" #name)); if (!gl.name) missing+=" gl" #name;
    GL_FUNCTION_LIST(X)
#undef X
    if (!missing.empty()) throw std::runtime_error("The OpenGL driver does not provide:"+missing+" (OpenGL 3.3 is required)");
}

//A mesh that lives in the GPU memory
//  VBO (vertex buffer):  the vertices, VERTEX_FLOATS floats each: x, y, z, u, v
//  EBO (element buffer): the vertex indices of every triangle (3 per triangle), so shared vertices are stored once
//  VAO (vertex array):   remembers how the VBO is read (which floats are the position and which are the uv) and which EBO goes with it
const int VERTEX_FLOATS=5;
struct Mesh{
    GLuint vao=0, vbo=0, ebo=0;
    GLsizei indexCount=0;
};

//Model data (the vertices are kept only in the GPU)
struct Model_set{
    Mesh mesh;
    bool hasUV=false;       //the vertices have texture coordinates (the .obj had "vt")
    bool uvTiles=false;     //some texture coordinate is outside 0..1 (the texture repeats); otherwise it never needs to wrap
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
std::array<double,3> g_windowColor={0.0,0.0,0.0}; //background color (0-255), changed with windowColor
bool g_frameStarted=false;             //the screen was already cleared for the frame that is being drawn
bool g_resizable=false;                //start was called with resizable=true

//Clears the screen with the window color. The python script never has to do it: it happens by itself once per frame,
//at the first render() of the frame (or at flip() if nothing was drawn, so the window still shows its color).
//A frame ends with flip(). The polygon mode of setWireframe is also applied here, so it changes at the start of a frame
void beginFrame(){
    if (g_frameStarted) return;
    glClearColor((float)(g_windowColor[0]*mul_color),(float)(g_windowColor[1]*mul_color),(float)(g_windowColor[2]*mul_color),1.0f);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glPolygonMode(GL_FRONT_AND_BACK,g_wireframe?GL_LINE:GL_FILL);
    g_frameStarted=true;
}

//---------------- Angles ----------------
//Every angle of the engine goes from 0 to 360 degrees. An angle that is given must be inside that range (360 is the same as 0),
//otherwise it is an error, and an angle that is read is always inside it too (so -30 is read as 330).
//For a negative angle use 360 - x, or x % 360 in python.
//The values of rotate() are NOT angles: they are how much to turn (a speed). They can be negative (turn to the other side)
//and bigger than 360 (more than a full turn), and it is the result that stays from 0 to 360
double wrapAngle(double degrees){
    //(floor instead of fmod: the fmod of glibc 2.38 would make the .so need that glibc version or newer)
    double r=degrees-360.0*std::floor(degrees/360.0);
    return r>=360.0?0.0:r; //a tiny negative angle can round up to exactly 360
}
double checkAngle(double degrees,const std::string& what){
    if (!std::isfinite(degrees) || degrees<0.0 || degrees>360.0){
        std::ostringstream msg;
        msg<<what<<" must be a number from 0 to 360 degrees (got "<<degrees<<"). For a negative angle use 360 - x, or x % 360";
        throw py::value_error(msg.str());
    }
    return degrees>=360.0?0.0:degrees; //360 is the same as 0
}

//How much to turn in rotate(): any finite number. nan and inf are errors because they would ruin the angle
double checkTurn(double degrees,const std::string& what){
    if (!std::isfinite(degrees)){
        std::ostringstream msg;
        msg<<what<<" must be a finite number (got "<<degrees<<")";
        throw py::value_error(msg.str());
    }
    return degrees;
}

//Rotation of a model: the three angles are checked before any of them is changed
void setModelRotation(Model_set& d,double x,double y,double z){
    const double v[3]={checkAngle(x,"rotation x"),checkAngle(y,"rotation y"),checkAngle(z,"rotation z")};
    for (int i=0; i<3; i++) d.rot[i]=(float)v[i];
}
void addModelRotation(Model_set& d,double x,double y,double z){
    const double v[3]={checkTurn(x,"rotate x"),checkTurn(y,"rotate y"),checkTurn(z,"rotate z")};
    for (int i=0; i<3; i++) d.rot[i]=(float)wrapAngle(d.rot[i]+wrapAngle(v[i])); //the turn is reduced first, so a huge value is still exact
}

//Camera that the python script moves
struct Camera{
    std::array<double,3> pos={0.0,0.0,5.0}; //x, y, z (starts away from the origin)
    float yaw=0.0f;    //horizontal rotation (degrees)
    float pitch=0.0f;  //vertical rotation (degrees)
    float fov=60.0f;
    bool dirty=true;   //the projection must be rebuilt (fov changed)

    void setPitch(float p){ pitch=p>89.0f?89.0f:(p<-89.0f?-89.0f:p); }
    void rotate(float dyaw,float dpitch){ yaw=(float)wrapAngle(yaw+dyaw); setPitch(pitch+dpitch); }
};
Camera g_camera;

//4x4 matrix in doubles, in the column order OpenGL uses: m[column*4+row]. It starts as the identity.
//The matrices are built in doubles on the CPU (once per model) and converted to float only to be sent to the GPU
struct Mat4{
    double m[16];
    Mat4(){ for (int i=0; i<16; i++) m[i]=(i%5==0)?1.0:0.0; }
    Mat4 operator*(const Mat4& b) const{
        Mat4 r;
        for (int c=0; c<4; c++) for (int row=0; row<4; row++){
            double sum=0.0;
            for (int k=0; k<4; k++) sum+=m[k*4+row]*b.m[c*4+k];
            r.m[c*4+row]=sum;
        }
        return r;
    }
};
Mat4 matScale(double x,double y,double z){ Mat4 r; r.m[0]=x; r.m[5]=y; r.m[10]=z; return r; }
Mat4 matTranslate(double x,double y,double z){ Mat4 r; r.m[12]=x; r.m[13]=y; r.m[14]=z; return r; }
Mat4 matRotX(double deg){ double a=deg*pi_val/180.0, c=cos(a), s=sin(a); Mat4 r; r.m[5]=c; r.m[9]=-s; r.m[6]=s; r.m[10]=c; return r; }
Mat4 matRotY(double deg){ double a=deg*pi_val/180.0, c=cos(a), s=sin(a); Mat4 r; r.m[0]=c; r.m[8]=s; r.m[2]=-s; r.m[10]=c; return r; }
Mat4 matRotZ(double deg){ double a=deg*pi_val/180.0, c=cos(a), s=sin(a); Mat4 r; r.m[0]=c; r.m[4]=-s; r.m[1]=s; r.m[5]=c; return r; }
Mat4 g_proj; //perspective projection (rebuilt by applyProjection when the fov changes)

//Copies a mesh to the GPU: VBO + EBO, and a VAO that describes how to read them. Needs the OpenGL context
//vertexFloats = number of floats in vertexData (VERTEX_FLOATS per vertex), indexCount = number of indices (3 per triangle)
void destroyMesh(Mesh& mesh){
    if (mesh.ebo!=0) gl.DeleteBuffers(1,&mesh.ebo);
    if (mesh.vbo!=0) gl.DeleteBuffers(1,&mesh.vbo);
    if (mesh.vao!=0) gl.DeleteVertexArrays(1,&mesh.vao);
    mesh=Mesh();
}

bool uploadMesh(Mesh& mesh,const float* vertexData,size_t vertexFloats,const uint32_t* indices,size_t indexCount){
    while (glGetError()!=GL_NO_ERROR){} //forget old errors, so only the ones from this upload are checked
    destroyMesh(mesh);
    gl.GenVertexArrays(1,&mesh.vao);
    gl.GenBuffers(1,&mesh.vbo);
    gl.GenBuffers(1,&mesh.ebo);

    gl.BindVertexArray(mesh.vao); //everything below is recorded in this VAO

    gl.BindBuffer(GL_ARRAY_BUFFER,mesh.vbo);
    gl.BufferData(GL_ARRAY_BUFFER,(GLsizeiptr)(vertexFloats*sizeof(float)),vertexData,GL_STATIC_DRAW);

    gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER,mesh.ebo); //the VAO remembers which EBO is bound
    gl.BufferData(GL_ELEMENT_ARRAY_BUFFER,(GLsizeiptr)(indexCount*sizeof(uint32_t)),indices,GL_STATIC_DRAW);

    const GLsizei stride=(GLsizei)(VERTEX_FLOATS*sizeof(float));
    gl.EnableVertexAttribArray(0); //location 0 of the vertex shader: position (x,y,z)
    gl.VertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,stride,(const void*)0);
    gl.EnableVertexAttribArray(1); //location 1: texture coordinate (u,v)
    gl.VertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,stride,(const void*)(3*sizeof(float)));

    gl.BindVertexArray(0);
    gl.BindBuffer(GL_ARRAY_BUFFER,0);

    mesh.indexCount=(GLsizei)indexCount;
    if (glGetError()!=GL_NO_ERROR){ destroyMesh(mesh); return false; }
    return true;
}

//The pipeline: a vertex shader + a fragment shader linked into one program that runs on the GPU
//Vertex shader:   position = uMVP * vertex (uMVP = projection * camera * model, one matrix for the whole model)
//Fragment shader: color = shade color (* texture); only pixels with uAlphaRange.x < alpha <= uAlphaRange.y are drawn
const char* VERTEX_SHADER_SRC=R"GLSL(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV;
uniform mat4 uMVP;
out vec2 vUV;
void main(){
    gl_Position=uMVP*vec4(aPos,1.0);
    vUV=vec2(aUV.x,1.0-aUV.y); //.obj has v=0 at the bottom, images at the top
}
)GLSL";

const char* FRAGMENT_SHADER_SRC=R"GLSL(#version 330 core
in vec2 vUV;
uniform sampler2D uTex;
uniform int uUseTex;
uniform vec3 uShade;
uniform vec2 uAlphaRange; //a pixel is only drawn if uAlphaRange.x < alpha <= uAlphaRange.y ((-1, 2) = every pixel)
out vec4 FragColor;
void main(){
    vec4 color=vec4(uShade,1.0);
    if (uUseTex!=0) color*=texture(uTex,vUV);
    if (color.a<=uAlphaRange.x || color.a>uAlphaRange.y) discard;
    FragColor=color;
}
)GLSL";

struct Pipeline{
    GLuint program=0;
    GLint uMVP=-1, uTex=-1, uUseTex=-1, uShade=-1, uAlphaRange=-1;

    static GLuint compile(GLenum type,const char* source){
        GLuint shader=gl.CreateShader(type);
        gl.ShaderSource(shader,1,&source,nullptr);
        gl.CompileShader(shader);
        GLint ok=0;
        gl.GetShaderiv(shader,GL_COMPILE_STATUS,&ok);
        if (!ok){
            char log[2048]; GLsizei len=0;
            gl.GetShaderInfoLog(shader,sizeof(log),&len,log);
            gl.DeleteShader(shader);
            throw std::runtime_error(std::string(type==GL_VERTEX_SHADER?"Vertex":"Fragment")+" shader error: "+std::string(log,len));
        }
        return shader;
    }

    //Compiles and links a vertex shader and a fragment shader into a program (used by the 3D pipeline and by the HUD pipeline)
    static GLuint makeProgram(const char* vertexSource,const char* fragmentSource){
        GLuint vs=compile(GL_VERTEX_SHADER,vertexSource);
        GLuint fs=0;
        try{ fs=compile(GL_FRAGMENT_SHADER,fragmentSource); }
        catch(...){ gl.DeleteShader(vs); throw; }

        GLuint prog=gl.CreateProgram();
        gl.AttachShader(prog,vs);
        gl.AttachShader(prog,fs);
        gl.LinkProgram(prog);
        gl.DeleteShader(vs); //the program keeps them until it is deleted
        gl.DeleteShader(fs);
        GLint ok=0;
        gl.GetProgramiv(prog,GL_LINK_STATUS,&ok);
        if (!ok){
            char log[2048]; GLsizei len=0;
            gl.GetProgramInfoLog(prog,sizeof(log),&len,log);
            gl.DeleteProgram(prog);
            throw std::runtime_error("Shader link error: "+std::string(log,len));
        }
        return prog;
    }

    void build(){
        program=makeProgram(VERTEX_SHADER_SRC,FRAGMENT_SHADER_SRC);

        uMVP=gl.GetUniformLocation(program,"uMVP");
        uTex=gl.GetUniformLocation(program,"uTex");
        uUseTex=gl.GetUniformLocation(program,"uUseTex");
        uShade=gl.GetUniformLocation(program,"uShade");
        uAlphaRange=gl.GetUniformLocation(program,"uAlphaRange");

        gl.UseProgram(program);
        gl.Uniform1i(uTex,0); //the texture is always on texture unit 0
    }

    void use(){ gl.UseProgram(program); }
    void destroy(){ if (program!=0 && g_context) gl.DeleteProgram(program); program=0; }
};
Pipeline g_pipeline;

//The HUD pipeline: draws 2D images over the screen. It is separate from the 3D pipeline because a HUD is not a 3D model:
//no camera, no projection matrix, no depth. All the work of placing the image is done here, in the vertex shader:
//the corners of a 0..1 square (the same 4 vertices for every HUD) are scaled to the size of the image, rotated around its
//center, moved to the position and then converted from window pixels to the -1..1 screen space of OpenGL
const char* HUD_VERTEX_SHADER_SRC=R"GLSL(#version 330 core
layout(location=1) in vec2 aCorner; //corner of the image: (0,0) = top-left, (1,1) = bottom-right
uniform vec2 uPos;    //top-left corner of the image in window pixels (y goes down, like the mouse)
uniform vec2 uSize;   //size in pixels
uniform vec2 uRot;    //cosine and sine of the rotation (counterclockwise) around the center of the image
uniform vec2 uScreen; //size of the window in pixels
out vec2 vUV;
void main(){
    vec2 local=(aCorner-0.5)*uSize;                                                  //from the center of the image
    vec2 turned=vec2(local.x*uRot.x+local.y*uRot.y, local.y*uRot.x-local.x*uRot.y); //counterclockwise on a screen where y goes down
    vec2 pixel=uPos+0.5*uSize+turned;                                                //position in the window
    gl_Position=vec4(pixel.x/uScreen.x*2.0-1.0, 1.0-pixel.y/uScreen.y*2.0, 0.0, 1.0);
    vUV=aCorner; //the first row of the image is the top: no flip needed
}
)GLSL";

const char* HUD_FRAGMENT_SHADER_SRC=R"GLSL(#version 330 core
in vec2 vUV;
uniform sampler2D uTex;
uniform float uOpacity; //0..1, multiplies the alpha
uniform int uUseAlpha;  //1 = the alpha of the image counts, 0 = the image is drawn opaque
out vec4 FragColor;
void main(){
    vec4 color=texture(uTex,vUV);
    float alpha=(uUseAlpha!=0?color.a:1.0)*uOpacity;
    FragColor=vec4(color.rgb,alpha);
}
)GLSL";

struct HudPipeline{
    GLuint program=0;
    GLint uPos=-1, uSize=-1, uRot=-1, uScreen=-1, uTex=-1, uOpacity=-1, uUseAlpha=-1;

    void build(){
        program=Pipeline::makeProgram(HUD_VERTEX_SHADER_SRC,HUD_FRAGMENT_SHADER_SRC);
        uPos=gl.GetUniformLocation(program,"uPos");
        uSize=gl.GetUniformLocation(program,"uSize");
        uRot=gl.GetUniformLocation(program,"uRot");
        uScreen=gl.GetUniformLocation(program,"uScreen");
        uTex=gl.GetUniformLocation(program,"uTex");
        uOpacity=gl.GetUniformLocation(program,"uOpacity");
        uUseAlpha=gl.GetUniformLocation(program,"uUseAlpha");
        gl.UseProgram(program);
        gl.Uniform1i(uTex,0); //the image is always on texture unit 0
    }
    void use(){ gl.UseProgram(program); }
    void destroy(){ if (program!=0 && g_context) gl.DeleteProgram(program); program=0; }
};
HudPipeline g_hudPipeline;
Mesh g_hudQuad; //the square that every HUD is drawn with: 4 vertices and 2 triangles, created once in start()

//Asks main.py for the model and sends it to the GPU
bool loadModel(const std::string& path,Model_set&m){
    try{
        py::module_ loader=loaderModule();
        py::tuple res=loader.attr(PY_LOAD_OBJ)(path);

        //res = (vertex_data, indices, has_uv)
        //  vertex_data: float32, VERTEX_FLOATS per vertex (x,y,z,u,v)    -> VBO
        //  indices:     uint32, 3 per triangle (main.py already split the faces) -> EBO
        auto vdata=res[0].cast<py::array_t<float,py::array::c_style|py::array::forcecast>>();
        auto idx=res[1].cast<py::array_t<uint32_t,py::array::c_style|py::array::forcecast>>();
        bool hasUV=res[2].cast<bool>();

        size_t vertexCount=(size_t)vdata.size()/VERTEX_FLOATS;
        if (vdata.size()%VERTEX_FLOATS!=0 || vertexCount==0 || idx.size()==0 || idx.size()%3!=0){
            std::cout<<"The model has no valid triangles\n";
            return false;
        }
        const uint32_t* ip=idx.data();
        for (py::ssize_t i=0; i<idx.size(); i++){
            if (ip[i]>=vertexCount){ std::cout<<"The model has a triangle that points to a vertex that does not exist\n"; return false; }
        }

        //If every texture coordinate is inside 0..1 the texture never has to repeat, and not repeating avoids a line
        //on the edges of the model (the smoothing of the texture would mix in the pixels of the opposite edge)
        const float* vp=vdata.data();
        bool tiles=false;
        for (size_t i=0; i<vertexCount && !tiles; i++){
            float u=vp[i*VERTEX_FLOATS+3], v=vp[i*VERTEX_FLOATS+4];
            if (u<-0.0001f || u>1.0001f || v<-0.0001f || v>1.0001f) tiles=true;
        }
        m.uvTiles=tiles;

        if (!uploadMesh(m.mesh,vdata.data(),(size_t)vdata.size(),ip,(size_t)idx.size())){
            std::cout<<"The GPU could not store the model\n";
            return false;
        }
        m.hasUV=hasUV;
        return true;
    }catch(const py::error_already_set& e){
        std::cout<<"Python error: "<<e.what()<<"\n";
        return false;
    }
}

//Asks main.py for the image and sends it to the GPU (needs the OpenGL context to exist). Used by models and by HUDs
//wrap = what happens outside 0..1: GL_REPEAT (the texture repeats) or GL_CLAMP_TO_EDGE (the last pixel is kept)
bool loadTextureInto(const std::string& path,GLuint& texture,int& texW,int& texH,GLint wrap){
    try{
        py::module_ loader=loaderModule();
        auto img=loader.attr(PY_LOAD_TEXTURE)(path).cast<py::array_t<uint8_t,py::array::c_style|py::array::forcecast>>();
        int h=(int)img.shape(0);
        int w=(int)img.shape(1);
        texW=w; texH=h;

        glGenTextures(1,&texture);
        glBindTexture(GL_TEXTURE_2D,texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT,1);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR); //GL_NEAREST for a pixelated look
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,wrap);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,wrap);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,img.data());
        return true;
    }catch(const py::error_already_set& e){
        std::cout<<"Python error: "<<e.what()<<"\n";
        return false;
    }
}

bool loadTexture(const std::string& path,Model_set&m,GLint wrap=GL_CLAMP_TO_EDGE){
    return loadTextureInto(path,m.texture,m.texW,m.texH,wrap);
}

//Builds a flat 2D model (like a sheet of paper) for a texture, used when loadModel gets a texture but no .obj
//It lies on the XY plane, centered on the origin and facing +Z (towards a camera that looks down -Z).
//Its height is 1 unit and its width keeps the proportion of the image, so the picture is never stretched.
//There is no face culling in drawModel, so both sides of the paper are visible.
//pos, scale and rotation work exactly like in any other model
void makePlane(Model_set&m){
    float aspect=(m.texW>0 && m.texH>0)?(float)m.texW/(float)m.texH:1.0f;
    float hw=aspect*0.5f, hh=0.5f;

    const float vertices[4*VERTEX_FLOATS]={
        //x    y    z     u     v   (same uv convention as .obj: v=0 at the bottom)
        -hw, -hh, 0.0f, 0.0f, 0.0f,
         hw, -hh, 0.0f, 1.0f, 0.0f,
         hw,  hh, 0.0f, 1.0f, 1.0f,
        -hw,  hh, 0.0f, 0.0f, 1.0f
    };
    const uint32_t indices[6]={0,1,2, 0,2,3}; //two triangles
    if (!uploadMesh(m.mesh,vertices,4*VERTEX_FLOATS,indices,6)) throw std::runtime_error("The GPU could not store the plane");
    m.hasUV=true;
}

void applyProjection(); //defined below, next to the other engine functions

//Draws the model with one draw call. The position of every vertex is calculated by the vertex shader on the GPU;
//the CPU only builds the matrix: model (scale -> rotate X, then Y, then Z -> position relative to the camera),
//then the camera rotation (pitch, then yaw), then the projection
void drawModel(Model_set& m){

    //A plane made without model and without texture has nothing to show through alpha, so
    //transparent=true makes the whole plane invisible (and it does not hide other models)
    beginFrame(); //clears the screen if this is the first thing drawn in the frame
    if (m.blank && m.transparent) return;
    if (m.mesh.vao==0 || m.mesh.indexCount==0) return; //unloaded
    if (g_camera.dirty) applyProjection();

    //The position relative to the camera is calculated in doubles, so far away models do not lose precision
    Mat4 model=matTranslate(m.pos[0]-g_camera.pos[0],m.pos[1]-g_camera.pos[1],m.pos[2]-g_camera.pos[2])
              *matRotZ(m.rot[2])*matRotY(m.rot[1])*matRotX(m.rot[0])
              *matScale(m.scale[0],m.scale[1],m.scale[2]);
    Mat4 view=matRotX(g_camera.pitch)*matRotY(g_camera.yaw);
    Mat4 mvp=g_proj*view*model;
    float mvpf[16];
    for (int i=0; i<16; i++) mvpf[i]=(float)mvp.m[i];

    //Only textures if there is an image and the vertices have uv
    bool useTex=m.texture!=0 && m.hasUV;
    //Transparency (only for textured models) is drawn in two passes, so it does not depend on the order of the render() calls:
    //  1) the (almost) opaque pixels, drawn normally and writing the depth
    //  2) the semi-transparent pixels (soft edges, glass): blended with what is already on the screen but NOT writing the depth.
    //     A pixel that does not write depth cannot hide a model that is drawn after it, so no border of the window color is left
    //     around the model. Fully transparent pixels are not drawn at all.
    //Several semi-transparent models (glass over glass) still need to be drawn from the farthest to the nearest
    bool useAlpha=m.transparent && useTex;
    const float OPAQUE_ALPHA=0.99f;   //alpha above this counts as opaque
    const float CLEAR_ALPHA=0.01f;    //alpha up to this counts as fully transparent

    g_pipeline.use();
    gl.UniformMatrix4fv(g_pipeline.uMVP,1,GL_FALSE,mvpf);
    gl.Uniform3f(g_pipeline.uShade,g_shade[0],g_shade[1],g_shade[2]);
    gl.Uniform1i(g_pipeline.uUseTex,useTex?1:0);

    if (useTex) glBindTexture(GL_TEXTURE_2D,m.texture);
    gl.BindVertexArray(m.mesh.vao);

    if (!useAlpha){
        gl.Uniform2f(g_pipeline.uAlphaRange,-1.0f,2.0f);   //every pixel, alpha is ignored
        glDrawElements(GL_TRIANGLES,m.mesh.indexCount,GL_UNSIGNED_INT,(const void*)0);
    }else{
        //Pass 1: opaque pixels
        gl.Uniform2f(g_pipeline.uAlphaRange,OPAQUE_ALPHA,2.0f);
        glDrawElements(GL_TRIANGLES,m.mesh.indexCount,GL_UNSIGNED_INT,(const void*)0);

        //Pass 2: semi-transparent pixels (blended, no depth writing)
        gl.Uniform2f(g_pipeline.uAlphaRange,CLEAR_ALPHA,OPAQUE_ALPHA);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glDrawElements(GL_TRIANGLES,m.mesh.indexCount,GL_UNSIGNED_INT,(const void*)0);

        //Puts the state back so the next model is not affected
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }
    gl.BindVertexArray(0);
}

//---------------- HUD: 2D images over the screen ----------------
//A HUD is not a 3D model: it has no vertices of its own, ignores the camera and the depth buffer, and goes through its own
//pipeline (g_hudPipeline). Coordinates are window pixels with (0,0) at the top-left and y going down, the same as mousePosXY()
struct Hud_set{
    GLuint texture=0;             //OpenGL texture id (0 = unloaded)
    int imgW=0, imgH=0;           //size of the image in pixels
    double pos[2]={0.0,0.0};      //top-left corner of the image (before the rotation)
    double size[2]={0.0,0.0};     //size in pixels (it starts as the size of the image)
    double rot=0.0;               //degrees, counterclockwise, around the center of the image
    float opacity=1.0f;           //0 = invisible, 1 = as the image is
    bool transparent=true;        //the alpha channel of the image counts (false = the image is drawn opaque)
};

//Draws the HUD over what is already on the screen, with one draw call. It is drawn in the order of the render() calls:
//what is rendered later is on top, so render the HUD after the 3D models of the frame
void drawHud(Hud_set& h){
    if (!g_glAlive) return;
    beginFrame(); //clears the screen if this is the first thing drawn in the frame
    if (h.texture==0 || g_hudQuad.vao==0) return; //unloaded
    double w=h.size[0], hgt=h.size[1];
    if (w<=0.0 || hgt<=0.0 || h.opacity<=0.0f) return; //nothing to see

    double angle=h.rot*pi_val/180.0;
    g_hudPipeline.use();
    gl.Uniform2f(g_hudPipeline.uPos,(float)h.pos[0],(float)h.pos[1]);
    gl.Uniform2f(g_hudPipeline.uSize,(float)w,(float)hgt);
    gl.Uniform2f(g_hudPipeline.uRot,(float)cos(angle),(float)sin(angle));
    gl.Uniform2f(g_hudPipeline.uScreen,(float)w_x,(float)w_y);
    gl.Uniform1f(g_hudPipeline.uOpacity,h.opacity);
    gl.Uniform1i(g_hudPipeline.uUseAlpha,h.transparent?1:0);

    //A HUD is always on top of the 3D scene, filled (even with setWireframe) and blended with what is behind it
    glDisable(GL_DEPTH_TEST);
    glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);

    glBindTexture(GL_TEXTURE_2D,h.texture);
    gl.BindVertexArray(g_hudQuad.vao);
    glDrawElements(GL_TRIANGLES,g_hudQuad.indexCount,GL_UNSIGNED_INT,(const void*)0);
    gl.BindVertexArray(0);

    //Puts the state back so the 3D models that come next are not affected
    glDisable(GL_BLEND);
    glPolygonMode(GL_FRONT_AND_BACK,g_wireframe?GL_LINE:GL_FILL);
    glEnable(GL_DEPTH_TEST);
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

    //Frees everything the model has in the GPU (texture, VBO, EBO and VAO). Only possible while the OpenGL context exists.
    //After this the model is not drawn anymore
    void unload(){
        if (g_glAlive){
            if (data.texture!=0) glDeleteTextures(1,&data.texture);
            destroyMesh(data.mesh);
        }
        data.texture=0;
        data.mesh=Mesh();
    }
};
std::vector<std::weak_ptr<Model>> g_models; //every model that was loaded, so quit() can free them
size_t g_modelsPruneAt=64;                  //when g_models reaches this size the entries of released models are removed

//A HUD that python holds (a 2D image over the screen). Its texture is freed when python releases it or when the engine quits
class Hud{
public:
    Hud_set data;

    Hud()=default;
    Hud(const Hud&)=delete;
    Hud& operator=(const Hud&)=delete;
    ~Hud(){ unload(); }

    void render(){ drawHud(data); }

    //Frees the image from the GPU (only possible while the OpenGL context exists). After this the HUD is not drawn anymore
    void unload(){
        if (g_glAlive && data.texture!=0) glDeleteTextures(1,&data.texture);
        data.texture=0;
    }
};
std::vector<std::weak_ptr<Hud>> g_huds;     //every HUD that was loaded, so quit() can free them
size_t g_hudsPruneAt=64;

//Python frees a model (or a HUD) when nothing refers to it anymore, but its entry in the list stays until it is removed here.
//Without this the list (and the memory of everything that was ever created) would grow forever.
//It only runs when the list doubles, so the cost per loaded object stays tiny
template<class T>
void forgetReleased(std::vector<std::weak_ptr<T>>& list,size_t& pruneAt){
    if (list.size()<pruneAt) return;
    list.erase(std::remove_if(list.begin(),list.end(),[](const std::weak_ptr<T>& w){ return w.expired(); }),list.end());
    pruneAt=std::max<size_t>(64,list.size()*2);
}
void forgetReleasedModels(){ forgetReleased(g_models,g_modelsPruneAt); }
void forgetReleasedHuds(){ forgetReleased(g_huds,g_hudsPruneAt); }

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

//Rebuilds the perspective projection matrix (60 degrees fov by default)
void applyProjection(){
    double aspect=(double)w_x/w_y, nearP=0.1, farP=5000.0;
    double top=nearP*tan(g_camera.fov*pi_val/360.0);
    double right=top*aspect;
    Mat4 p;
    p.m[0]=nearP/right;
    p.m[5]=nearP/top;
    p.m[10]=-(farP+nearP)/(farP-nearP);
    p.m[14]=-2.0*farP*nearP/(farP-nearP);
    p.m[11]=-1.0;
    p.m[15]=0.0;
    g_proj=p;
    g_camera.dirty=false;
}

//Functions that python can call BlueEngine

//Frame rate: -1 = the default (vsync: the monitor refresh rate), 0 = no limit, above 0 = limit in frames per second
double g_targetFps=-1.0;
std::chrono::steady_clock::time_point g_nextFrame=std::chrono::steady_clock::now();

//Vsync only in the default mode: with a limit (or no limit) it is off, so the limit can be above the monitor refresh rate
void applyFrameSettings(){
    if (g_context) SDL_GL_SetSwapInterval(g_targetFps<0?1:0);
    g_nextFrame=std::chrono::steady_clock::now();
}

//Waits so that frames do not start faster than g_targetFps (called after each flip)
void limitFrameRate(){
    if (g_targetFps<=0) return;
    using clock=std::chrono::steady_clock;
    g_nextFrame+=std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0/g_targetFps));
    if (g_nextFrame<=clock::now()){ g_nextFrame=clock::now(); return; } //already late: do not try to catch up
    py::gil_scoped_release release; //other python threads can work while this one waits
    std::this_thread::sleep_until(g_nextFrame-std::chrono::microseconds(500));
    while (clock::now()<g_nextFrame){} //the last half millisecond is waited in a loop, because sleeping is not that precise
}

//Closes the OpenGL context and the window
void destroyWindow(){
    if (g_context){ SDL_GL_DeleteContext(g_context); g_context=nullptr; }
    if (g_window){ SDL_DestroyWindow(g_window); g_window=nullptr; SDL_Quit(); }
}

//Reads the real size of the window (it changes when the player resizes it or goes fullscreen) and adapts the drawing to it
void updateWindowSize(){
    int w=0,h=0;
    SDL_GL_GetDrawableSize(g_window,&w,&h);
    if (w>0 && h>0){ w_x=w; w_y=h; }   //a minimized window can report 0
    glViewport(0,0,w_x,w_y);
    g_camera.dirty=true;               //the projection depends on the proportion of the window
}

//The square that every HUD is drawn with. Its "uv" are the corners (0,0) top-left .. (1,1) bottom-right of the image
void makeHudQuad(){
    const float vertices[4*VERTEX_FLOATS]={
        //x    y    z     u     v
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
        1.0f, 1.0f, 0.0f, 1.0f, 1.0f,
        0.0f, 1.0f, 0.0f, 0.0f, 1.0f
    };
    const uint32_t indices[6]={0,1,2, 0,2,3};
    if (!uploadMesh(g_hudQuad,vertices,4*VERTEX_FLOATS,indices,6)) throw std::runtime_error("The GPU could not store the HUD quad");
}

//Creates the window and the OpenGL context (3.3 core: there is no fixed pipeline, everything goes through the shaders)
//resizable=true: the player can resize the window, and F11 or Alt+Enter switch fullscreen on and off
void engineInit(int width,int height,const std::string& title,bool resizable){
    if (g_glAlive) throw std::runtime_error("BlueEngine.start was already called");
    w_x=width; w_y=height;
    g_resizable=resizable;
    g_frameStarted=false;

    if (SDL_Init(SDL_INIT_VIDEO)!=0) throw std::runtime_error(std::string("SDL error: ")+SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,24); //Depth buffer for 3D

    try{
        g_window=SDL_CreateWindow(title.c_str(),SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,w_x,w_y,SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN|(resizable?SDL_WINDOW_RESIZABLE:0));
        if (!g_window) throw std::runtime_error(std::string("Window error: ")+SDL_GetError());
        g_context=SDL_GL_CreateContext(g_window);
        if (!g_context) throw std::runtime_error(std::string("Context error (OpenGL 3.3 is required): ")+SDL_GetError());
        applyFrameSettings(); //Vsync, unless setFPS was used

        loadGLFunctions();
        g_pipeline.build();
        g_hudPipeline.build();
        makeHudQuad();
    }catch(...){
        destroyMesh(g_hudQuad);
        g_hudPipeline.destroy();
        g_pipeline.destroy();
        destroyWindow();
        SDL_Quit();
        throw;
    }
    updateWindowSize(); //the real size of the window
    glEnable(GL_DEPTH_TEST);

    g_glAlive=true;
    applyProjection();
}

//Frees the models and closes the window. Safe to call more than once
void engineQuit(){
    for (auto& w: g_models) if (auto m=w.lock()) m->unload();
    g_models.clear();
    g_modelsPruneAt=64;
    for (auto& w: g_huds) if (auto h=w.lock()) h->unload();
    g_huds.clear();
    g_hudsPruneAt=64;
    if (g_glAlive) destroyMesh(g_hudQuad); else g_hudQuad=Mesh();
    g_hudPipeline.destroy();
    g_pipeline.destroy();
    g_glAlive=false;
    g_frameStarted=false;
    g_resizable=false;
    destroyWindow();
}

//Raw input for main.py: this only reads SDL. The processing (pressed keys, mouse movement,
//closing the window, time between frames) is done in main.py

//Switches between the window and the fullscreen (the desktop resolution is kept, so there is no screen flicker)
void toggleFullscreen(){
    if (!g_window) return;
    bool full=(SDL_GetWindowFlags(g_window)&SDL_WINDOW_FULLSCREEN)!=0;
    SDL_SetWindowFullscreen(g_window,full?0:SDL_WINDOW_FULLSCREEN_DESKTOP);
}

//Events of this frame as tuples (kind, a, b):
//("quit",0,0)  ("keydown",scancode,repeat)  ("keyup",scancode,0)
//("mouse",xrel,yrel)  ("mousebuttondown",button,0)  ("wheel",x,y)
py::list engineRawEvents(){
    requireGL();
    py::list out;
    SDL_Event event;
    while (SDL_PollEvent(&event)){
        if (event.type==SDL_QUIT) out.append(py::make_tuple("quit",0,0));
        else if (event.type==SDL_KEYDOWN){
            //The engine handles these two keys itself (only in a resizable window); the script still receives them
            bool altEnter=event.key.keysym.scancode==SDL_SCANCODE_RETURN && (event.key.keysym.mod&KMOD_ALT)!=0;
            if (g_resizable && event.key.repeat==0 && (event.key.keysym.scancode==SDL_SCANCODE_F11 || altEnter)) toggleFullscreen();
            out.append(py::make_tuple("keydown",(int)event.key.keysym.scancode,(int)event.key.repeat));
        }
        else if (event.type==SDL_WINDOWEVENT){
            if (event.window.event==SDL_WINDOWEVENT_SIZE_CHANGED) updateWindowSize();
        }
        else if (event.type==SDL_KEYUP) out.append(py::make_tuple("keyup",(int)event.key.keysym.scancode,0));
        else if (event.type==SDL_MOUSEMOTION) out.append(py::make_tuple("mouse",(int)event.motion.xrel,(int)event.motion.yrel));
        else if (event.type==SDL_MOUSEBUTTONDOWN) out.append(py::make_tuple("mousebuttondown",(int)event.button.button,0));
        else if (event.type==SDL_MOUSEWHEEL){
            //y > 0: the wheel was rolled forward (away from the player), y < 0: backward; x > 0: to the right.
            //With "natural scrolling" the system flips these numbers (direction FLIPPED): they are flipped back here,
            //so the script always gets the real movement of the wheel
            int sign=event.wheel.direction==SDL_MOUSEWHEEL_FLIPPED?-1:1;
            out.append(py::make_tuple("wheel",sign*(int)event.wheel.x,sign*(int)event.wheel.y));
        }
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

bool engineMouseHeld(int button){
    requireGL();
    if (button<1 || button>5) return false;
    return (SDL_GetMouseState(nullptr,nullptr)&SDL_BUTTON(button))!=0;
}

std::tuple<int,int> engineMousePosXY(){
    requireGL();
    int x=0,y=0;
    SDL_GetMouseState(&x,&y);
    return std::make_tuple(x,y);
}

void engineSetMouseCapture(bool on){ requireGL(); SDL_SetRelativeMouseMode(on?SDL_TRUE:SDL_FALSE); }
bool engineMouseCaptured(){ requireGL(); return SDL_GetRelativeMouseMode()==SDL_TRUE; }

//Sets the color of the window background (0-255 each). The screen is cleared automatically with it at the start of every frame.
//It can be called before start(); inside a frame it is used from the next frame
void engineWindowColor(std::array<double,3> color){
    g_windowColor=color;
}

//Shows what was drawn
void engineFlip(){
    requireGL();
    beginFrame(); //a frame where nothing was drawn still shows the window color
    glFlush();
    SDL_GL_SwapWindow(g_window);
    g_frameStarted=false; //the next frame starts with a clear screen
    limitFrameRate();
}

//Sets the frame rate: a number = limit in frames per second, 0 = no limit, nothing = the default (vsync)
//It can be called before or after start()
void engineSetFPS(const py::object& fps){
    if (fps.is_none()){
        g_targetFps=-1.0;
    }else{
        if (py::isinstance<py::bool_>(fps) || !PyNumber_Check(fps.ptr()))
            throw py::type_error("setFPS needs a number (frames per second), 0 for no limit, or no value for the default (vsync)");
        double value=fps.cast<double>();
        if (!std::isfinite(value) || value<0.0)
            throw py::value_error("setFPS needs a number >= 0 (0 = no limit)");
        g_targetFps=value;
    }
    applyFrameSettings();
}

//Versions of what the C++ side depends on (used by ver() in main.py). The OpenGL ones only exist after start(): None before it
#if defined(__clang__)
#define BLUE_COMPILER __VERSION__
#elif defined(__GNUC__)
#define BLUE_COMPILER "GCC " __VERSION__
#else
#define BLUE_COMPILER "unknown compiler"
#endif
py::dict engineVersions(){
    py::dict d;
    SDL_version linked;   SDL_GetVersion(&linked);   //the SDL that is running
    SDL_version compiled; SDL_VERSION(&compiled);    //the SDL of the headers used to build this file
    d["sdl_linked"]=py::make_tuple((int)linked.major,(int)linked.minor,(int)linked.patch);
    d["sdl_compiled"]=py::make_tuple((int)compiled.major,(int)compiled.minor,(int)compiled.patch);
    d["pybind11"]=py::make_tuple(PYBIND11_VERSION_MAJOR,PYBIND11_VERSION_MINOR,PYBIND11_VERSION_PATCH);
    d["compiler"]=std::string(BLUE_COMPILER);
    d["cpp"]=(long)__cplusplus;
    auto gl=[](GLenum name)->py::object{
        const GLubyte* s=glGetString(name);
        return s?py::object(py::str(reinterpret_cast<const char*>(s))):py::object(py::none());
    };
    d["opengl"]=g_glAlive?gl(GL_VERSION):py::object(py::none());
    d["glsl"]=g_glAlive?gl(GL_SHADING_LANGUAGE_VERSION):py::object(py::none());
    d["renderer"]=g_glAlive?gl(GL_RENDERER):py::object(py::none());
    d["vendor"]=g_glAlive?gl(GL_VENDOR):py::object(py::none());
    return d;
}

//Name of the GPU that OpenGL is really using, for example "NVIDIA GeForce RTX 3060/PCIe/SSE2".
//Something like "llvmpipe" means that the CPU is drawing instead of a graphics card
std::string engineGpuName(){
    requireGL();
    const GLubyte* name=glGetString(GL_RENDERER);
    return name?std::string(reinterpret_cast<const char*>(name)):std::string("unknown");
}

//Loads a model (and its texture, if any) to memory
//If obj is empty but texture is not, it creates a flat plane (like a sheet of paper) with that texture
//If both are empty, it creates a blank 1x1 plane (shade color). With transparent=true that plane is invisible
//transparent=true makes the alpha channel of the texture count (PNG with transparent areas); false ignores it
std::shared_ptr<Model> engineLoadModel(const std::string& obj,const std::string& texture,
                                       std::array<double,3> pos,const py::object& scale,std::array<double,3> rotation,bool transparent){
    requireGL();
    auto md=std::make_shared<Model>();
    for (int i=0; i<3; i++) md->data.pos[i]=pos[i];
    setModelRotation(md->data,rotation[0],rotation[1],rotation[2]); //0 to 360 each, otherwise ValueError
    setScaleFrom(md->data,scale);
    md->data.transparent=transparent;

    if (obj.empty()){
        if (texture.empty()){
            //Blank plane: 1x1 square with no texture, drawn with the shade color.
            //With transparent=true it is invisible (it still exists: it can be moved, scaled, rotated and turned visible later)
            md->data.blank=true;
        }else{
            //Plane mode: the texture is the whole point, so failing to load it is an error
            //(the plane never needs the texture to repeat, so its edges are clamped)
            if (!loadTexture(texture,md->data,GL_CLAMP_TO_EDGE)) throw std::runtime_error("Could not load the texture: "+texture);
        }
        makePlane(md->data);
    }else{
        if (!loadModel(obj,md->data)) throw std::runtime_error("Could not load the model: "+obj);
        if (!texture.empty() && !loadTexture(texture,md->data,md->data.uvTiles?GL_REPEAT:GL_CLAMP_TO_EDGE)) std::cout<<"No texture loaded, drawing without it\n";
    }

    forgetReleasedModels();
    g_models.push_back(md);
    return md;
}

//Loads an image to be drawn over the screen (HUD). Position and size are in window pixels, (0,0) = top-left corner of the window
//size = None: the size of the image. rotation: degrees, counterclockwise, around the center
//transparent=true: the alpha channel of the image counts (PNG with transparent areas); false: the image is drawn opaque
std::shared_ptr<Hud> engineLoadHud(const std::string& image,std::array<double,2> pos,const std::optional<std::array<double,2>>& size,
                                   double rotation,double opacity,bool transparent){
    requireGL();
    if (image.empty()) throw std::runtime_error("loadHud needs the path of an image");
    const double rot=checkAngle(rotation,"rotation"); //0 to 360, checked before the image is loaded
    auto hud=std::make_shared<Hud>();
    Hud_set& d=hud->data;
    if (!loadTextureInto(image,d.texture,d.imgW,d.imgH,GL_CLAMP_TO_EDGE)) throw std::runtime_error("Could not load the image: "+image);

    d.pos[0]=pos[0]; d.pos[1]=pos[1];
    if (size){
        if (!(size->at(0)>=0.0) || !(size->at(1)>=0.0)) throw py::value_error("The size of a HUD cannot be negative");
        d.size[0]=size->at(0); d.size[1]=size->at(1);
    }else{
        d.size[0]=d.imgW; d.size[1]=d.imgH;
    }
    d.rot=rot;
    d.opacity=(float)std::min(1.0,std::max(0.0,opacity));
    d.transparent=transparent;

    forgetReleasedHuds();
    g_huds.push_back(hud);
    return hud;
}

PYBIND11_MODULE(BlueEngine,m){
    m.doc()="BlueEngine: window, input, camera and models controlled from python";
    py::module_::import("atexit").attr("register")(py::cpp_function(&engineQuit));

    //Window and frame (start, quit, running, stop, flip and deltaTime are written in main.py and use these.
    //flip() is there because it also reads the events of the frame: _flip only shows what was drawn)
    m.def("_start",&engineInit,py::arg("width"),py::arg("height"),py::arg("title"),py::arg("resizable")=false);
    m.def("_quit",&engineQuit);
    m.def("windowTitle",[](const std::string& t){ requireGL(); SDL_SetWindowTitle(g_window,t.c_str()); });
    m.def("windowColor",&engineWindowColor,py::arg("color")=std::make_tuple(0.0,0.0,0.0),
          "windowColor((r,g,b)): color of the window background (0-255); the screen is cleared with it automatically");
    m.def("_flip",&engineFlip);
    m.def("setFPS",&engineSetFPS,py::arg("fps")=py::none(),
          "setFPS(fps): limit in frames per second; 0 = no limit; no value = the default (vsync, the monitor refresh rate)");
    m.def("gpuName",&engineGpuName,"Name of the GPU that is really drawing (after start)");
    m.def("_versions",&engineVersions); //raw data for ver() in main.py

    //Render settings
    m.def("setShade",[](std::array<double,3> c){ for (int i=0; i<3; i++) g_shade[i]=(float)(c[i]*mul_color); },py::arg("color"));
    m.def("setWireframe",[](bool on){ g_wireframe=on; },py::arg("on"));

    //Input: raw SDL data for main.py (keyDown, keyPressed, mousePos... are written in main.py)
    m.def("_pollRaw",&engineRawEvents);
    m.def("_keyHeld",&engineKeyHeld,py::arg("scancode"));
    m.def("_scancode",&engineScancode,py::arg("name"));
    m.def("_mouseHeld",&engineMouseHeld,py::arg("button"));
    m.def("_mousePosXY",&engineMousePosXY);
    m.def("_ticks",[](){ return (double)SDL_GetPerformanceCounter()*1000.0/(double)SDL_GetPerformanceFrequency(); }); //ms, with decimals
    m.def("setMouseCap",&engineSetMouseCapture,py::arg("on"));
    m.def("mouseCap",&engineMouseCaptured);

    //Camera
    //Camera: called with values it sets them (camera.pos(0,0,5)), called without values it returns the current ones (camera.pos())
    py::class_<Camera>(m,"Camera")
        .def("pos",[](const Camera& c){ return std::make_tuple(c.pos[0],c.pos[1],c.pos[2]); })
        .def("pos",[](Camera& c,double x,double y,double z){ c.pos={x,y,z}; },py::arg("x"),py::arg("y"),py::arg("z"))
        .def("pos",[](Camera& c,std::array<double,3> v){ c.pos=v; },py::arg("xyz"))
        //Angles go from 0 to 360 (anything else is an error). yaw: 90 = turned to the right, 270 = to the left
        .def("yaw",[](const Camera& c){ return (float)wrapAngle(c.yaw); })
        .def("yaw",[](Camera& c,double degrees){ c.yaw=(float)checkAngle(degrees,"yaw"); },py::arg("degrees"))
        //pitch: 1 to 89 = looking down, 271 to 359 = looking up (the camera stops at those limits: 90 to 180 becomes 89, 181 to 270 becomes 271)
        .def("pitch",[](const Camera& c){ return (float)wrapAngle(c.pitch); })
        .def("pitch",[](Camera& c,double degrees){
                 double d=checkAngle(degrees,"pitch");
                 c.setPitch((float)(d>180.0?d-360.0:d)); },py::arg("degrees"))
        //fov: the opening of the view, greater than 0 and less than 180
        .def("fov",[](const Camera& c){ return c.fov; })
        .def("fov",[](Camera& c,double degrees){
                 if (!(degrees>0.0 && degrees<180.0)){
                     std::ostringstream msg;
                     msg<<"fov must be a number greater than 0 and less than 180 degrees (got "<<degrees<<")";
                     throw py::value_error(msg.str());
                 }
                 c.fov=(float)degrees; c.dirty=true; },py::arg("degrees"))
        //Turns the camera by these amounts (they are not angles but how much to turn, so they can be negative and any size;
        //both are checked before anything changes). yaw: positive = to the right. pitch: positive = down (it stops at the limits)
        .def("rotate",[](Camera& c,double dyaw,double dpitch){
                 double y=checkTurn(dyaw,"yaw"), p=checkTurn(dpitch,"pitch");
                 c.rotate((float)wrapAngle(y),(float)std::max(-360.0,std::min(360.0,p))); },py::arg("yaw")=0.0,py::arg("pitch")=0.0);
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
        //rotation angles go from 0 to 360 degrees (anything else is an error)
        .def("rotation",[](Model& md,double x,double y,double z){ setModelRotation(md.data,x,y,z); },
             py::arg("x"),py::arg("y"),py::arg("z"))
        .def("rotation",[](Model& md,std::array<double,3> v){ setModelRotation(md.data,v[0],v[1],v[2]); },py::arg("xyz"))
        .def("move",[](Model& md,double x,double y,double z){ md.data.pos[0]+=x; md.data.pos[1]+=y; md.data.pos[2]+=z; },
             py::arg("x")=0.0,py::arg("y")=0.0,py::arg("z")=0.0)
        //turns the model by these amounts (how much to turn, not angles: negative and bigger than 360 are fine); the rotation read stays from 0 to 360
        .def("rotate",[](Model& md,double x,double y,double z){ addModelRotation(md.data,x,y,z); },
             py::arg("x")=0.0,py::arg("y")=0.0,py::arg("z")=0.0)
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

    //HUD: 2D images over the screen. Positions and sizes are in window pixels, (0,0) = top-left corner, y goes down (like mousePosXY)
    py::class_<Hud,std::shared_ptr<Hud>>(m,"Hud")
        //Called with values they set them (hud.pos(10,20)), called without values they return the current ones (hud.pos())
        .def("pos",[](const Hud& h){ return std::make_tuple(h.data.pos[0],h.data.pos[1]); })
        .def("pos",[](Hud& h,double x,double y){ h.data.pos[0]=x; h.data.pos[1]=y; },py::arg("x"),py::arg("y"))
        .def("pos",[](Hud& h,std::array<double,2> v){ h.data.pos[0]=v[0]; h.data.pos[1]=v[1]; },py::arg("xy"))
        //size in pixels (it starts as the size of the image)
        .def("size",[](const Hud& h){ return std::make_tuple(h.data.size[0],h.data.size[1]); })
        .def("size",[](Hud& h,double w,double hgt){
                 if (!(w>=0.0) || !(hgt>=0.0)) throw py::value_error("The size of a HUD cannot be negative");
                 h.data.size[0]=w; h.data.size[1]=hgt; },py::arg("width"),py::arg("height"))
        .def("size",[](Hud& h,std::array<double,2> v){
                 if (!(v[0]>=0.0) || !(v[1]>=0.0)) throw py::value_error("The size of a HUD cannot be negative");
                 h.data.size[0]=v[0]; h.data.size[1]=v[1]; },py::arg("wh"))
        .def("rotation",[](const Hud& h){ return h.data.rot; })
        .def("rotation",[](Hud& h,double degrees){ h.data.rot=checkAngle(degrees,"rotation"); },py::arg("degrees")) //0 to 360, counterclockwise
        .def("opacity",[](const Hud& h){ return h.data.opacity; })
        .def("opacity",[](Hud& h,double value){ h.data.opacity=(float)std::min(1.0,std::max(0.0,value)); },py::arg("value"))
        .def("transparent",[](const Hud& h){ return h.data.transparent; })
        .def("transparent",[](Hud& h,bool on){ h.data.transparent=on; },py::arg("on"))
        .def("move",[](Hud& h,double x,double y){ h.data.pos[0]+=x; h.data.pos[1]+=y; },py::arg("x")=0.0,py::arg("y")=0.0)
        .def("rotate",[](Hud& h,double degrees){ h.data.rot=wrapAngle(h.data.rot+wrapAngle(checkTurn(degrees,"rotate"))); },py::arg("degrees")) //how much to turn: any number, the rotation read stays from 0 to 360
        .def("imageSize",[](const Hud& h){ return std::make_tuple(h.data.imgW,h.data.imgH); }) //size of the image file in pixels
        //render() can also receive the parameters, and they stay changed for the next frames (None = keeps the current one):
        //hud.render(pos=(10,20), size=(64,64), rotation=45, opacity=0.5, transparent=True)
        //They are all checked before anything changes, so a wrong value leaves the HUD as it was and nothing is drawn.
        //As the HUD is drawn right away, the same HUD can be drawn in several places in one frame: icon.render((10,10)); icon.render((80,10))
        .def("render",[](Hud& h,const std::optional<std::array<double,2>>& pos,const std::optional<std::array<double,2>>& size,
                         const std::optional<double>& rotation,const std::optional<double>& opacity,const std::optional<bool>& transparent){
                 if (size && (!(size->at(0)>=0.0) || !(size->at(1)>=0.0))) throw py::value_error("The size of a HUD cannot be negative");
                 std::optional<double> rot;
                 if (rotation) rot=checkAngle(*rotation,"rotation");
                 if (pos){ h.data.pos[0]=pos->at(0); h.data.pos[1]=pos->at(1); }
                 if (size){ h.data.size[0]=size->at(0); h.data.size[1]=size->at(1); }
                 if (rot) h.data.rot=*rot;
                 if (opacity) h.data.opacity=(float)std::min(1.0,std::max(0.0,*opacity));
                 if (transparent) h.data.transparent=*transparent;
                 h.render(); },
             py::arg("pos")=py::none(),py::arg("size")=py::none(),py::arg("rotation")=py::none(),
             py::arg("opacity")=py::none(),py::arg("transparent")=py::none())
        .def("unload",&Hud::unload);

    m.def("loadHud",&engineLoadHud,
          py::arg("image"),
          py::arg("pos")=std::make_tuple(0.0,0.0),
          py::arg("size")=py::none(),
          py::arg("rotation")=0.0,
          py::arg("opacity")=1.0,
          py::arg("transparent")=true,
          "loadHud(image, pos=(0,0), size=None, rotation=0, opacity=1, transparent=True): a 2D image drawn over the screen with hud.render(). rotation: 0 to 360, counterclockwise");

    //Runs main.py last, so everything it uses from this module already exists
    loadEmbeddedMain(m);
}
