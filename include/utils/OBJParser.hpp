#pragma once

#include <fstream>
#include <vector>
#include <iostream>
#include <sstream>

#include "../math/Vec.hpp"

namespace valo {

struct OBJ {
    std::string             name;

    std::vector<Vec4f>      points;
    std::vector<Vec2f>      uvs;
    std::vector<Vec3f>      normals;
    std::vector<uint32_t>   indices;
};

inline OBJ parseOBJ(const std::string& path) {
    OBJ obj;

    std::ifstream objFile(path);
    if (!objFile.is_open()) {
        std::cerr << "Could not open file \"" << path << "\"." << std::endl;
        return {};
    }

    std::string line;
    while (std::getline(objFile, line)) {
        if (line.empty()) continue;

        std::string key;
        std::istringstream iss(line);
        iss >> key;

        if (key == "v") {
            float x, y, z;
            iss >> x >> y >> z;
            obj.points.emplace_back(
                x, y, z, 1.f
            );
        }

        else if (key == "vn") {
            float x, y, z;
            iss >> x >> y >> z;
            obj.normals.push_back(
                Vec3f(x, y, z).normalized()
            );
        }

        else if (key == "vt") {
            std::string u, v;
            iss >> u >> v;
            obj.uvs.emplace_back(
                u, v
            );
        }

        // else if (key == "f") {
        //     uint32_t 
        // }
    }

    return obj;
}

}