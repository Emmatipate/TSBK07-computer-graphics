// Part B: Many-bone worm

// New version by Ingemar 2010
// Removed all dependencies of the Wild Magic (wml) library.
// Replaced it with VectorUtils2 (in source)
// Replaced old shader module with the simpler "ShaderUtils" unit.

// 2013: Adapted to VectorUtils3 and MicroGlut.

// gcc skinning2.c ../common/*.c -lGL -o skinning2 -I../common
// not working any more. This is untested but closer to the truth:
// gcc skinning2.c -o skinning2 ../common/*.c ../common/Linux/MicroGlut.c -I../common -I../common/Linux -DGL_GLEXT_PROTOTYPES -lXt -lX11 -lGL -lm

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#ifdef __APPLE__
// Mac
#include <OpenGL/gl3.h>
//uses framework Cocoa
#else
#ifdef WIN32
// MS
#include <stdio.h>
#include <GL/glew.h>
#else
// Linux
#include <GL/gl.h>
#endif
#endif

#include "MicroGlut.h"
#include "GL_utilities.h"
#include "VectorUtils3.h"
#include "loadobj.h"
#include <string.h>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <string>
#include <map>
#include <vector>
using namespace std;

// Ref till shader
GLuint g_shader, g_mesh_shader;


// vec2 is mostly useful for texture coordinates, otherwise you don't use it much.
// That is why VectorUtils3 doesn't support it (yet)
typedef struct vec2
{
	GLfloat s, t;
}
vec2, *vec2Ptr;


typedef struct Triangle
{
	GLuint        v1;
	GLuint        v2;
	GLuint        v3;
} Triangle;

#define CYLINDER_SEGMENT_LENGTH 0.37
#define kMaxRow 100
#define kMaxCorners 8
#define kMaxBones 10
#define kMaxg_poly ((kMaxRow-1) * kMaxCorners * 2)
#ifndef Pi
#define Pi 3.1416
#endif
#ifndef true
#define true 1
#endif

#define BONE_LENGTH 4.0

Triangle g_poly[kMaxg_poly];

// vertices
vec3 g_vertsOrg[kMaxRow][kMaxCorners];
vec3 g_normalsOrg[kMaxRow][kMaxCorners];
vec3 g_vertsRes[kMaxRow][kMaxCorners];
vec3 g_normalsRes[kMaxRow][kMaxCorners];

// vertex attributes
int g_boneIDs[kMaxRow][kMaxCorners][4];
float g_boneWeights[kMaxRow][kMaxCorners][4];
vec2 g_boneWeightVis[kMaxRow][kMaxCorners]; // Copy data to here to visualize your weights

mat4 boneRestMatrices[kMaxBones];

Model *cylinderModel; // Collects all the above for drawing with glDrawElements

mat4 modelViewMatrix, projectionMatrix;

enum VB_TYPES {
    INDEX_BUFFER,
    POS_VB,
    NORMAL_VB,
    TEXCOORD_VB,
    BONE_VB,
    NUM_VBs
};

struct VertexBoneData {
	VertexBoneData() : index{0}, weight{0} {}
	GLuint index;
	GLfloat weight;
};

const uint BONES_PER_VERTEX = 4;

struct VertexData {
	vec3 position;
	vec3 normal;
	vec2 tex_coord;
	GLuint bone_indices[BONES_PER_VERTEX];
	GLfloat bone_weights[BONES_PER_VERTEX];
};

struct Mesh {
    GLuint VAO;
    GLuint buffer;
	GLuint index_buffer;
	struct MeshEntry {
		uint num_indices;
		uint base_vertex;
		uint base_index;
		uint material_index;
	};
	vector<MeshEntry> entries;
};

struct Mesh load_mesh(const aiScene* scene) {
	Mesh mesh;

	unsigned int num_vertices = 0;
	unsigned int num_indices = 0;

	mesh.entries.reserve(scene->mNumMeshes);

	struct BoneInfo {
        mat4 bone_offset;
        mat4 final_transformation;
    };

	vector<VertexData> vertices;
	vector<GLuint> indices;
	vector<BoneInfo> bones;
	for (uint i = 0; i < scene->mNumMeshes; i++) {
		aiMesh* submesh = scene->mMeshes[i];

		// Store buffer offsets for all vertex data
		Mesh::MeshEntry entry;
		entry.material_index = submesh->mMaterialIndex;
		entry.num_indices = submesh->mNumFaces * 3;

		entry.base_index = num_indices;
		entry.base_vertex = num_vertices;

		mesh.entries.push_back(entry);

		num_vertices += submesh->mNumVertices;
		num_indices += entry.num_indices;

		//vertices.reserve(submesh->mNumVertices);
		// Initialize vertex data
		for (uint j = 0; j < submesh->mNumVertices; j++) {
			aiVector3D pos = submesh->mVertices[j];
			aiVector3D norm = submesh->mNormals[j];
			aiVector3D tex_coord = submesh->mTextureCoords[0][j];

			VertexData vert;
			vert.position = SetVector(pos.x, pos.y, pos.z);
			vert.normal = SetVector(norm.x, norm.y, norm.z);
			vert.tex_coord = vec2 { tex_coord.x, tex_coord.y };

			vertices.push_back(vert);
		}

		//indices.reserve(submesh->mNumFaces * 3);
		for (uint j = 0; j < submesh->mNumFaces; j++) {
			aiFace face = submesh->mFaces[j];
			assert(face.mNumIndices == 3);
			indices.push_back(face.mIndices[0]);
			indices.push_back(face.mIndices[1]);
			indices.push_back(face.mIndices[2]);
		}

		map<string, uint> bone_mapping;
		uint num_bones = 0;
		// Initialize bones
		for (uint j = 0; j < submesh->mNumBones; j++) {
			struct aiBone* bone = submesh->mBones[j];

			uint bone_index = 0;
			string bone_name(submesh->mName.data);

			if (bone_mapping.find(bone_name) == bone_mapping.end()) {
				// Bone was not found, assign a new index
				bone_index = num_bones;
				num_bones++;

				BoneInfo bi;
				// The data should be stored in the same way
				bi.bone_offset = *reinterpret_cast<mat4*>(&bone->mOffsetMatrix);
				bone_mapping[bone_name] = bone_index;
			} else {
				bone_index = bone_mapping[bone_name];
			}

			for (uint k = 0; k < bone->mNumWeights; k++) {
				uint vert_id = entry.base_vertex + bone->mWeights[k].mVertexId;
				float weight = bone->mWeights[k].mWeight;

				// Find an available bone
				for (uint l = 0; l < BONES_PER_VERTEX; l++) {
					if (vertices[vert_id].bone_weights[l] == 0.0f) {
						vertices[vert_id].bone_weights[l] = weight;
						vertices[vert_id].bone_indices[l] = bone_index;
					}
				}
			}
		}
	}

	glGenVertexArrays(1, &mesh.VAO);
	glBindVertexArray(mesh.VAO);

	glGenBuffers(1, &mesh.buffer);

	glBindBuffer(GL_ARRAY_BUFFER, mesh.buffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices[0]) * vertices.size(), &vertices[0], GL_STATIC_DRAW);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.index_buffer);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices[0]) * indices.size(), &indices[0], GL_STATIC_DRAW);

	GLuint pos_loc = glGetAttribLocation(g_mesh_shader, "in_Position");
	GLuint norm_loc = glGetAttribLocation(g_mesh_shader, "in_Normal");
	GLuint texcoord_loc = glGetAttribLocation(g_mesh_shader, "in_TexCoord");
	GLuint boneid_loc = glGetAttribLocation(g_mesh_shader, "in_BoneIDs");
	GLuint weight_loc = glGetAttribLocation(g_mesh_shader, "in_Weights");

	glEnableVertexAttribArray(pos_loc);
	glEnableVertexAttribArray(norm_loc);
	glEnableVertexAttribArray(texcoord_loc);
	glEnableVertexAttribArray(boneid_loc);
	glEnableVertexAttribArray(weight_loc);

	glVertexAttribPointer(pos_loc, 3, GL_FLOAT, GL_FALSE, sizeof(VertexData), 0);
	glVertexAttribPointer(norm_loc, 3, GL_FLOAT, GL_FALSE, sizeof(VertexData), (void*)offsetof(VertexData, normal));
	glVertexAttribPointer(texcoord_loc, 2, GL_FLOAT, GL_FALSE, sizeof(VertexData), (void*)offsetof(VertexData, tex_coord));
	glVertexAttribIPointer(boneid_loc, BONES_PER_VERTEX, GL_INT, sizeof(VertexData), (void*)offsetof(VertexData, bone_indices));
	glVertexAttribPointer(pos_loc, BONES_PER_VERTEX, GL_FLOAT, GL_FALSE, sizeof(VertexData), (void*)offsetof(VertexData, bone_weights));

	return mesh;
}

///////////////////////////////////////////////////
//		I N I T  B O N E  W E I G H T S
// Desc:  initierar benvikterna
//
void initBoneWeights(void) {
	long row, corner;
	int bone;

	// sätter värden till alla vertexar i meshen
	for (row = 0; row < kMaxRow; row++) {
		for (corner = 0; corner < kMaxCorners; corner++) {
			float boneWeights[kMaxBones];
			float totalBoneWeight = 0.f;

			float maxBoneWeight = 0.f;

			for (bone = 0; bone < kMaxBones; bone++) {
				float bonePos = BONE_LENGTH * bone;
				float boneDist = fabs(bonePos - g_vertsOrg[row][corner].x);
				float boneWeight = (BONE_LENGTH - boneDist) / (BONE_LENGTH);
				if (boneWeight < 0)
					boneWeight = 0;
				boneWeights[bone] = boneWeight;
				totalBoneWeight += boneWeight;

				if (maxBoneWeight < boneWeight)
					maxBoneWeight = boneWeight;
			}

			// Set all ids and weights to 0 so unused data doesn't destroy anything
			for (int bone_id = 0; bone_id < 4; bone_id++) {
				g_boneIDs[row][corner][bone_id] = 0;
				g_boneWeights[row][corner][bone_id] = 0;
			}

			int bone_i = 0;
			g_boneWeightVis[row][corner].s = 0;
			g_boneWeightVis[row][corner].t = 0;
			//printf("for vert %d %d: ", row, corner);
			for (bone = 0; bone < kMaxBones; bone++) {
				// Use the first 4 bones that have any weight for this vertex
				float weight = boneWeights[bone] / totalBoneWeight;
				if (weight > 0 && bone_i < 4) {
					g_boneIDs[row][corner][bone_i] = bone;
					g_boneWeights[row][corner][bone_i] = weight;
					//printf("b %d, w %f ", bone, weight);
					bone_i++;
				}
				if (bone & 1) g_boneWeightVis[row][corner].s += weight; // Copy data to here to visualize your weights or anything else
				if ((bone+1) & 1) g_boneWeightVis[row][corner].t += weight; // Copy data to here to visualize your weights
			}
			//printf("\n");
		}
	}

	// Build Model from cylinder data
	cylinderModel = LoadDataToModel(
		(GLfloat*) g_vertsRes,
		(GLfloat*) g_normalsRes,
		(GLfloat*) g_boneWeightVis, // texCoords
		NULL, // (GLfloat*) g_boneWeights, // colors
		(GLuint*) g_poly, // indices
		kMaxRow*kMaxCorners,
		kMaxg_poly * 3);

	glBindVertexArray(cylinderModel->vao);

	// Buffer bone IDs
	GLuint boneIdBuffer;
	glGenBuffers(1, &boneIdBuffer);
	glBindBuffer(GL_ARRAY_BUFFER, boneIdBuffer);
	glBufferData(GL_ARRAY_BUFFER, kMaxRow*kMaxCorners*sizeof(GLint)*4, g_boneIDs, GL_STATIC_DRAW);
	GLint loc = glGetAttribLocation(g_shader, "in_BoneIDs");
	if (loc >= 0) {
		glVertexAttribIPointer(loc, 4, GL_INT, 0, 0);
		glEnableVertexAttribArray(loc);
	}
	else
		fprintf(stderr, "%s warning: '%s' not found in shader!\n", "Bone ids", "in_BoneIDs");

	// Buffer bone weights
	GLuint boneWeightBuffer;
	glGenBuffers(1, &boneWeightBuffer);
	glBindBuffer(GL_ARRAY_BUFFER, boneWeightBuffer);
	glBufferData(GL_ARRAY_BUFFER, kMaxRow*kMaxCorners*sizeof(GLfloat)*4, g_boneWeights, GL_STATIC_DRAW);

	loc = glGetAttribLocation(g_shader, "in_Weights");
	if (loc >= 0) {
		glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, 0, 0);
		glEnableVertexAttribArray(loc);
	}
	else
		fprintf(stderr, "%s warning: '%s' not found in shader!\n", "Bone weights", "in_Weights");
}



///////////////////////////////////////////////////
//		B U I L D  C Y L I N D E R
// Desc:  bygger upp cylindern
//
void BuildCylinder()
{
	long	row, corner, cornerIndex;

	// sätter värden till alla vertexar i meshen
	for (row = 0; row < kMaxRow; row++) {
		for (corner = 0; corner < kMaxCorners; corner++) {
			g_vertsOrg[row][corner].x = (float) row * CYLINDER_SEGMENT_LENGTH;
			g_vertsOrg[row][corner].y = cos(corner * 2*Pi / kMaxCorners);
			g_vertsOrg[row][corner].z = sin(corner * 2*Pi / kMaxCorners);

			g_normalsOrg[row][corner].x = 0;
			g_normalsOrg[row][corner].y = cos(corner * 2*Pi / kMaxCorners);
			g_normalsOrg[row][corner].z = sin(corner * 2*Pi / kMaxCorners);
		}
	}

	// g_poly definerar mellan vilka vertexar som
	// trianglarna ska ritas
	for (row = 0; row < kMaxRow-1; row++) {
		for (corner = 0; corner < kMaxCorners; corner++) {
			// Quads built from two triangles

			if (corner < kMaxCorners-1) {
				cornerIndex = row * kMaxCorners + corner;
				g_poly[cornerIndex * 2].v1 = cornerIndex;
				g_poly[cornerIndex * 2].v2 = cornerIndex + 1;
				g_poly[cornerIndex * 2].v3 = cornerIndex + kMaxCorners + 1;

				g_poly[cornerIndex * 2 + 1].v1 = cornerIndex;
				g_poly[cornerIndex * 2 + 1].v2 = cornerIndex + kMaxCorners + 1;
				g_poly[cornerIndex * 2 + 1].v3 = cornerIndex + kMaxCorners;
			} else {
				// Specialfall: sista i varvet, gå runt hörnet korrekt
				cornerIndex = row * kMaxCorners + corner;
				g_poly[cornerIndex * 2].v1 = cornerIndex;
				g_poly[cornerIndex * 2].v2 = cornerIndex + 1 - kMaxCorners;
				g_poly[cornerIndex * 2].v3 = cornerIndex + kMaxCorners + 1 - kMaxCorners;

				g_poly[cornerIndex * 2 + 1].v1 = cornerIndex;
				g_poly[cornerIndex * 2 + 1].v2 = cornerIndex + kMaxCorners + 1 - kMaxCorners;
				g_poly[cornerIndex * 2 + 1].v3 = cornerIndex + kMaxCorners;
			}
		}
	}

	// lägger en kopia av orginal modellen i g_vertsRes
	memcpy(g_vertsRes,  g_vertsOrg, kMaxRow * kMaxCorners* sizeof(vec3));
	memcpy(g_normalsRes,  g_normalsOrg, kMaxRow * kMaxCorners* sizeof(vec3));
}


//////////////////////////////////////
//		B O N E
// Desc:	en enkel ben-struct med en
//			pos-vektor och en rot-vektor
//			rot vektorn skulle lika gärna
//			kunna vara av 3x3 men VectorUtils2 har bara 4x4
typedef struct Bone {
	vec3 pos;
	mat4 rot;
} Bone;


///////////////////////////////////////
//		G _ B O N E S
// vårt skelett
Bone g_bones[kMaxBones]; // Ursprungsdata, ändra ej
Bone g_bonesRes[kMaxBones]; // Animerat

mat4 boneTranslation(int b) {
	if (b == 0) {
		return T(0, 0, 0);
	} else {
		return T(BONE_LENGTH, 0, 0);
	}
}

mat4 TByVec(vec3 pos) {
	return T(pos.x, pos.y, pos.z);
}


///////////////////////////////////////////////////////
//		S E T U P  B O N E S
//
void setupBones(void)
{
	int bone;

	for (bone = 0; bone < kMaxBones; bone++) {
		g_bones[bone].pos = SetVector((float) bone * BONE_LENGTH, 0.0f, 0.0f);
		g_bones[bone].rot = IdentityMatrix();
		
		boneRestMatrices[bone] = T(-bone * BONE_LENGTH, 0.0f, 0.0f);
	}
}

///////////////////////////////////////////////////////
//		D E F O R M  C Y L I N D E R
//
// Desc:	deformera cylinder-meshen enligt skelettet
void DeformCylinder() {
	mat4 boneLocalAnimMatrices[kMaxBones];
	for (int b = 0; b < kMaxBones; ++b) {
		mat4 translation = boneTranslation(b);
		boneLocalAnimMatrices[b] = Mult(translation, g_bonesRes[b].rot);
	}

	mat4 boneAnimMatrices[kMaxBones];
	boneAnimMatrices[0] = boneLocalAnimMatrices[0];
	for(int b = 1; b < kMaxBones; ++b) {
		boneAnimMatrices[b] = Mult(
			boneAnimMatrices[b-1],
			boneLocalAnimMatrices[b]
		);
	}

	mat4 completeMatrix[kMaxBones];
	for (int b = 0; b < kMaxBones; ++b) {
		completeMatrix[b] = Mult(boneAnimMatrices[b], boneRestMatrices[b]);
	}

	GLint loc = glGetUniformLocation(g_shader, "bones");
	if (loc >= 0) {
		glUniformMatrix4fv(loc, kMaxBones, GL_TRUE, (const GLfloat*)completeMatrix);
	}
	else
		fprintf(stderr, "%s warning: '%s' not found in shader!\n", "Bone weights", "in_Weights");
}


/////////////////////////////////////////////
//		A N I M A T E  B O N E S
// Desc:	en väldigt enkel animation av skelettet
//			vrider ben 1 i en sin(counter)
void animateBones(void)
{
	int bone;
	// Hur mycket kring varje led? ändra gärna.
	float angleScales[10] = { 1, 1, 1, 1, 1, -1, -1, -1, -1, -1 };

	float time = glutGet(GLUT_ELAPSED_TIME) / 1000.0;
	// Hur mycket skall vi vrida?
	float angle = sin(time * 3.f) / 2.0f;

	memcpy(&g_bonesRes, &g_bones, kMaxBones*sizeof(Bone));

	g_bonesRes[0].rot = Rz(angle * angleScales[0]);

	for (bone = 1; bone < kMaxBones; bone++)
	g_bonesRes[bone].rot = Rz(angle * angleScales[bone]);
}


///////////////////////////////////////////////
//		S E T  B O N E  R O T A T I O N
// Desc:	sätter bone rotationen i vertex shadern
// (Ej obligatorisk.)
void setBoneRotation(void)
{
}


///////////////////////////////////////////////
//		 S E T  B O N E  L O C A T I O N
// Desc:	sätter bone positionen i vertex shadern
// (Ej obligatorisk.)
void setBoneLocation(void)
{
}


///////////////////////////////////////////////
//		 D R A W  C Y L I N D E R
// Desc:	sätter bone positionen i vertex shadern
void DrawCylinder()
{
	animateBones();

	// ---------=========  UPG 2 (extra) ===========---------
	// ersätt DeformCylinder med en vertex shader som gör vad DeformCylinder gör.
	// begynelsen till shaderkoden ligger i filen "ShaderCode.vert" ...
	//

	DeformCylinder();

	// setBoneLocation();
	// setBoneRotation();

	DrawModel(cylinderModel, g_shader, "in_Position", "in_Normal", "in_TexCoord");
}

Mesh g_mesh;

void DisplayWindow()
{
	mat4 m;

	glClearColor(0.4, 0.4, 0.2, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// Cylinder
	glUseProgram(g_shader);
	glBindVertexArray(cylinderModel->vao);
	m = Mult(projectionMatrix, modelViewMatrix);
	glUniformMatrix4fv(glGetUniformLocation(g_shader, "matrix"), 1, GL_TRUE, m.m);

	DrawCylinder();

	// Guard
	glUseProgram(g_mesh_shader);
	glBindVertexArray(g_mesh.VAO);
	glUniformMatrix4fv(glGetUniformLocation(g_mesh_shader, "matrix"), 1, GL_TRUE, m.m);

	for (uint i = 0; i < 1; i++) {
		// TODO: handle material

		//printf("before draw: num_indices %u, base_index %u, base_vertex %u\n", g_mesh.entries[i].num_indices, g_mesh.entries[i].base_index, g_mesh.entries[i].base_vertex);
		glDrawElementsBaseVertex(
			GL_TRIANGLES,
			g_mesh.entries[i].num_indices,
			GL_UNSIGNED_INT,
			(void*)(sizeof(uint) * g_mesh.entries[i].base_index),
			g_mesh.entries[i].base_vertex
		);
		//printf("after draw\n");
	}

	glutSwapBuffers();
};

void OnTimer(int value)
{
	glutPostRedisplay();
	glutTimerFunc(20, &OnTimer, value);
}

void keyboardFunc( unsigned char key, int x, int y)
{
	if(key == 27)	//Esc
	exit(1);
}

void reshape(GLsizei w, GLsizei h)
{
	vec3 cam = {0,0,40};
	vec3 look = {10,0,0};

	glViewport(0, 0, w, h);
	GLfloat ratio = (GLfloat) w / (GLfloat) h;
	projectionMatrix = perspective(90, ratio, 0.1, 1000);
	modelViewMatrix = lookAt(cam.x, cam.y, cam.z,
		look.x, look.y, look.z,
		0,1,0);
}

/////////////////////////////////////////
//		M A I N
//
int main(int argc, char **argv)
{
	glutInit(&argc, argv);

	glutInitWindowSize(512, 512);
	glutInitDisplayMode(GLUT_RGB | GLUT_DOUBLE | GLUT_DEPTH);
	glutInitContextVersion(3, 2); // Might not be needed in Linux
	glutCreateWindow("Them bones, them bones");

	glutDisplayFunc(DisplayWindow);
	glutTimerFunc(20, &OnTimer, 0);
	glutKeyboardFunc(keyboardFunc);
	glutReshapeFunc(reshape);

	g_shader = loadShaders("shader.vert" , "shader.frag");
	g_mesh_shader = loadShaders("mesh_shader.vert" , "mesh_shader.frag");

	// Set up depth buffer
	glEnable(GL_DEPTH_TEST);

	// initiering
#ifdef WIN32
	glewInit();
#endif


	Assimp::Importer importer;
	const aiScene* scene = importer.ReadFile(
		"boblampclean.md5mesh",
		aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_FlipUVs
	);
	g_mesh = load_mesh(scene);
	printf("loaded mesh\n");
	BuildCylinder();
	setupBones();
	initBoneWeights();

	glutMainLoop();
	return 0;
}