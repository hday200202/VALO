#pragma once

#include <fstream>
#include <vector>
#include <iostream>
#include <sstream>
#include <tiny_obj_loader.h>
#include <filesystem>

#include "../math/Vec.hpp"

namespace valo {

struct FaceVert { int v = -1, vt = -1, vn = -1; };  // -1 -> absent

struct MTLMaterial {
    std::string name;
    Vec3f diffuse, specular, emission;
    float shininess, opacity;
    std::string diffuseTex;
    std::string normalTex;
    std::string metallicTex;
    std::string roughnessTex;
    std::string emissiveTex;
};

struct OBJ {
    std::vector<Vec3f>      positions;
    std::vector<Vec2f>      uvs;
    std::vector<Vec3f>      normals;
    std::vector<FaceVert>   faces;
    std::vector<MTLMaterial>   materials;
    std::vector<int>        faceMTLMaterials;
};


/*
    Pass in a path to a .obj file or a path to a directory
    containing a .obj file if it contains materials.
*/
inline OBJ parseOBJ(const std::string& path) {
    namespace fs = std::filesystem;
    OBJ obj;
    fs::path input(path);
    std::string parentPath;
    std::string objFilePath;

    if (fs::is_directory(path)) {
        // look for .obj file
        bool found = false;
        for (const auto& entry : fs::directory_iterator(input)) {
            if (entry.path().extension() == ".obj") {
                objFilePath = entry.path().string();
                parentPath = path;
                found = true;
                break;
            }
        }

        if (!found) {
            std::cerr << "No object in directory \"" << path << "\". " << std::endl;
            return obj;
        }
    }
    else {
        objFilePath = path;
        parentPath = input.parent_path().string();
    }
    
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;

    std::string err;

    bool ret = tinyobj::LoadObj(
        &attrib, 
        &shapes, 
        &materials, 
        &err, 
        objFilePath.c_str(),
        (parentPath + "/").c_str()
    );

    if (!ret) { 
        std::cerr << "tiny_obj_loader Error: " << err << std::endl; 
        return obj; 
    }

    // Eventually gonna set these up with memcpy, since my
    // Vec*s are contiguous with no padding.
    for (size_t i = 2; i < attrib.vertices.size(); i += 3) {
        float x = attrib.vertices[i - 2];
        float y = attrib.vertices[i - 1];
        float z = attrib.vertices[i - 0];
        obj.positions.emplace_back(x, y, z);
    }

    for (size_t i = 2; i < attrib.normals.size(); i += 3) {
        float n1 = attrib.normals[i - 2];
        float n2 = attrib.normals[i - 1];
        float n3 = attrib.normals[i - 0];
        obj.normals.emplace_back(n1, n2, n3);
    }

    for (size_t i = 1; i < attrib.texcoords.size(); i += 2) {
        float u = attrib.texcoords[i - 1];
        float v = attrib.texcoords[i - 0];
        obj.uvs.emplace_back(u, v);
    }

    for (auto& shape : shapes) {
        for (auto& idx : shape.mesh.indices) {
            obj.faces.emplace_back(
                idx.vertex_index, 
                idx.texcoord_index, 
                idx.normal_index
            );
        }

        for (int id : shape.mesh.material_ids)
            obj.faceMTLMaterials.push_back(id);
    }

    for (auto& material : materials) {
        auto mat = MTLMaterial{
            .name = material.name,

            .diffuse = {
                material.diffuse[0], 
                material.diffuse[1], 
                material.diffuse[2]
            },

            .specular = {
                material.specular[0], 
                material.specular[1], 
                material.specular[2]
            },

            .emission = {
                material.emission[0], 
                material.emission[1], 
                material.emission[2]
            },

            .shininess      = material.shininess,
            .opacity        = material.dissolve,

            .diffuseTex     = material.diffuse_texname.empty() ? 
                "" : parentPath + "/" + material.diffuse_texname,

            .normalTex      = material.bump_texname.empty() ? 
                material.normal_texname.empty() ? 
                    "" : parentPath + "/" + material.normal_texname : 
                parentPath + "/" + material.bump_texname,

            .metallicTex    = material.metallic_texname.empty() ? 
                "" : parentPath + "/" + material.metallic_texname,

            .roughnessTex   = material.roughness_texname.empty() ? 
                "" : parentPath + "/" + material.roughness_texname,

            .emissiveTex    = material.emissive_texname.empty() ? 
                "" : parentPath + "/" + material.emissive_texname,
        };

        obj.materials.push_back(mat);
    }

    return obj;
}

}