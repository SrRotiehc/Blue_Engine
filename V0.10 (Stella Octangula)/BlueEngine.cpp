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

//---------------- How a model reaches the screen (GPU pipeline) ----------------
//Load time, once per model (the CPU only copies data):
//  main.py (or makePlane) gives the vertices (x,y,z,u,v) and the triangle indices -> they are copied to the GPU:
//    VBO = the vertices, EBO = the triangle indices, VAO = remembers how both are read
//Every frame, for each model.render():
//  CPU: builds ONE matrix (projection * camera * model position/rotation/scale) and sends it as a uniform
//  GPU, vertex shader:   position = matrix * vertex          (this used to be a loop over every vertex on the CPU)
//  GPU, fragment shader: color = shade color * texture, discarding transparent pixels when transparent=true
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
//Fragment shader: color = shade color (* texture), and pixels with alpha <= uAlphaCut are discarded
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
uniform float uAlphaCut; //pixels with alpha <= this are discarded (-1 = never)
out vec4 FragColor;
void main(){
    vec4 color=vec4(uShade,1.0);
    if (uUseTex!=0) color*=texture(uTex,vUV);
    if (color.a<=uAlphaCut) discard;
    FragColor=color;
}
)GLSL";

struct Pipeline{
    GLuint program=0;
    GLint uMVP=-1, uTex=-1, uUseTex=-1, uShade=-1, uAlphaCut=-1;

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

    void build(){
        GLuint vs=compile(GL_VERTEX_SHADER,VERTEX_SHADER_SRC);
        GLuint fs=0;
        try{ fs=compile(GL_FRAGMENT_SHADER,FRAGMENT_SHADER_SRC); }
        catch(...){ gl.DeleteShader(vs); throw; }

        program=gl.CreateProgram();
        gl.AttachShader(program,vs);
        gl.AttachShader(program,fs);
        gl.LinkProgram(program);
        gl.DeleteShader(vs); //the program keeps them until it is deleted
        gl.DeleteShader(fs);
        GLint ok=0;
        gl.GetProgramiv(program,GL_LINK_STATUS,&ok);
        if (!ok){
            char log[2048]; GLsizei len=0;
            gl.GetProgramInfoLog(program,sizeof(log),&len,log);
            gl.DeleteProgram(program); program=0;
            throw std::runtime_error("Shader link error: "+std::string(log,len));
        }

        uMVP=gl.GetUniformLocation(program,"uMVP");
        uTex=gl.GetUniformLocation(program,"uTex");
        uUseTex=gl.GetUniformLocation(program,"uUseTex");
        uShade=gl.GetUniformLocation(program,"uShade");
        uAlphaCut=gl.GetUniformLocation(program,"uAlphaCut");

        gl.UseProgram(program);
        gl.Uniform1i(uTex,0); //the texture is always on texture unit 0
    }

    void use(){ gl.UseProgram(program); }
    void destroy(){ if (program!=0 && g_context) gl.DeleteProgram(program); program=0; }
};
Pipeline g_pipeline;

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

//Asks main.py for the image and sends it to the GPU (needs the OpenGL context to exist)
//wrap = what happens outside 0..1: GL_REPEAT (the texture repeats) or GL_CLAMP_TO_EDGE (the last pixel is kept)
bool loadTexture(const std::string& path,Model_set&m,GLint wrap=GL_CLAMP_TO_EDGE){
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
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,wrap);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,wrap);
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
    //Transparency (only for textured models): alpha blends the pixel with what is behind it, and fully transparent
    //pixels are discarded so they do not hide other models through the depth buffer.
    //For several semi-transparent models, draw them after the opaque ones and from the farthest to the nearest
    bool useAlpha=m.transparent && useTex;

    g_pipeline.use();
    gl.UniformMatrix4fv(g_pipeline.uMVP,1,GL_FALSE,mvpf);
    gl.Uniform3f(g_pipeline.uShade,g_shade[0],g_shade[1],g_shade[2]);
    gl.Uniform1i(g_pipeline.uUseTex,useTex?1:0);
    gl.Uniform1f(g_pipeline.uAlphaCut,useAlpha?0.01f:-1.0f);

    if (useTex) glBindTexture(GL_TEXTURE_2D,m.texture);
    if (useAlpha){
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    }

    gl.BindVertexArray(m.mesh.vao);
    glDrawElements(GL_TRIANGLES,m.mesh.indexCount,GL_UNSIGNED_INT,(const void*)0);
    gl.BindVertexArray(0);

    //Puts the state back so the next model is not affected
    if (useAlpha) glDisable(GL_BLEND);
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

//Python frees a model when nothing refers to it anymore, but its entry in g_models stays until it is removed here.
//Without this the list (and the memory of every model that was ever created) would grow forever.
//It only runs when the list doubles, so the cost per loaded model stays tiny
void forgetReleasedModels(){
    if (g_models.size()<g_modelsPruneAt) return;
    g_models.erase(std::remove_if(g_models.begin(),g_models.end(),
                                  [](const std::weak_ptr<Model>& w){ return w.expired(); }),g_models.end());
    g_modelsPruneAt=std::max<size_t>(64,g_models.size()*2);
}

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

//Creates the window and the OpenGL context (3.3 core: there is no fixed pipeline, everything goes through the shaders)
void engineInit(int width,int height,const std::string& title){
    if (g_glAlive) throw std::runtime_error("BlueEngine.start was already called");
    w_x=width; w_y=height;

    if (SDL_Init(SDL_INIT_VIDEO)!=0) throw std::runtime_error(std::string("SDL error: ")+SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,24); //Depth buffer for 3D

    try{
        g_window=SDL_CreateWindow(title.c_str(),SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,w_x,w_y,SDL_WINDOW_OPENGL|SDL_WINDOW_SHOWN);
        if (!g_window) throw std::runtime_error(std::string("Window error: ")+SDL_GetError());
        g_context=SDL_GL_CreateContext(g_window);
        if (!g_context) throw std::runtime_error(std::string("Context error (OpenGL 3.3 is required): ")+SDL_GetError());
        applyFrameSettings(); //Vsync, unless setFPS was used

        loadGLFunctions();
        g_pipeline.build();
    }catch(...){
        g_pipeline.destroy();
        destroyWindow();
        SDL_Quit();
        throw;
    }
    glViewport(0,0,w_x,w_y);
    glEnable(GL_DEPTH_TEST);

    g_glAlive=true;
    applyProjection();
}

//Frees the models and closes the window. Safe to call more than once
void engineQuit(){
    for (auto& w: g_models) if (auto m=w.lock()) m->unload();
    g_models.clear();
    g_modelsPruneAt=64;
    g_pipeline.destroy();
    g_glAlive=false;
    destroyWindow();
}

//Raw input for main.py: this only reads SDL. The processing (pressed keys, mouse movement,
//closing the window, time between frames) is done in main.py

//Events of this frame as tuples (kind, a, b):
//("quit",0,0)  ("keydown",scancode,repeat)  ("keyup",scancode,0)
//("mouse",xrel,yrel)  ("mousebuttondown",button,0)
py::list engineRawEvents(){
    requireGL();
    py::list out;
    SDL_Event event;
    while (SDL_PollEvent(&event)){
        if (event.type==SDL_QUIT) out.append(py::make_tuple("quit",0,0));
        else if (event.type==SDL_KEYDOWN) out.append(py::make_tuple("keydown",(int)event.key.keysym.scancode,(int)event.key.repeat));
        else if (event.type==SDL_KEYUP) out.append(py::make_tuple("keyup",(int)event.key.keysym.scancode,0));
        else if (event.type==SDL_MOUSEMOTION) out.append(py::make_tuple("mouse",(int)event.motion.xrel,(int)event.motion.yrel));
        else if (event.type==SDL_MOUSEBUTTONDOWN) out.append(py::make_tuple("mousebuttondown",(int)event.button.button,0));
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

//Clears the screen with a color (0-255). Call at the start of the drawing
//(the camera is applied by each model.render(), so it is always the current one)
void engineClear(std::array<double,3> color){
    requireGL();
    glClearColor(color[0]*mul_color,color[1]*mul_color,color[2]*mul_color,1.0f);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glPolygonMode(GL_FRONT_AND_BACK,g_wireframe?GL_LINE:GL_FILL);
}

//Shows what was drawn
void engineFlip(){
    requireGL();
    glFlush();
    SDL_GL_SwapWindow(g_window);
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

PYBIND11_MODULE(BlueEngine,m){
    m.doc()="BlueEngine: window, input, camera and models controlled from python";
    py::module_::import("atexit").attr("register")(py::cpp_function(&engineQuit));

    //Window and frame (start, quit, running, stop, windowClosed, pollEvents and deltaTime are written in main.py and use these)
    m.def("_start",&engineInit,py::arg("width"),py::arg("height"),py::arg("title"));
    m.def("_quit",&engineQuit);
    m.def("windowTitle",[](const std::string& t){ requireGL(); SDL_SetWindowTitle(g_window,t.c_str()); });
    m.def("clear",&engineClear,py::arg("color")=std::make_tuple(0.0,0.0,0.0));
    m.def("flip",&engineFlip);
    m.def("setFPS",&engineSetFPS,py::arg("fps")=py::none(),
          "setFPS(fps): limit in frames per second; 0 = no limit; no value = the default (vsync, the monitor refresh rate)");
    m.def("gpuName",&engineGpuName,"Name of the GPU that is really drawing (after start)");

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
