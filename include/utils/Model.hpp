#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include <utility>
#include <iostream>

#include <stb_image.h>

#include "../VALO.hpp"
#include "GLTFParser.hpp"

namespace valo {

/*
    A glTF model on the GPU, ready to draw:

        Model model = loadModel(valo, "path/to/scene.gltf");

        submitModel(valo, model, program, [&](const ModelDraw& d, const ModelMaterial& m) {
            return MyUniforms{ viewProj * d.world, d.world, m };
        });

    loadModel does what every viewer otherwise writes by hand: one vertex and
    index buffer for the file, every texture loaded once with the right
    encoding for its slot, tangents, bounds, and one draw per primitive per
    node with the cull/blend state its material asks for.
*/


/*
    One primitive of one mesh, placed by one node. cmd already names the
    geometry and its index range, and carries the cull, winding, blend and depth
    state the material asks for - copy it, set a program and uniforms, submit.
*/
struct ModelDraw {
    RenderCommand   cmd;
    Mat4f           world;              // the node's world transform
    uint32_t        material = 0;       // into Model::materials; always valid
};


struct Model {
    VertexBufferID              verts   = EMPTY_BUFFER;     // GLTFVertex: pos, normal, uv, tangent
    IndexBufferID               indices = EMPTY_BUFFER;
    std::vector<ModelMaterial>  materials;
    std::vector<ModelDraw>      draws;
    std::vector<TextureID>      textures;                   // owned; destroyModel frees them

    // world space, over every draw - for framing a camera on the model
    Vec3f boundsMin = {0.f, 0.f, 0.f};
    Vec3f boundsMax = {0.f, 0.f, 0.f};

    bool  loaded() const { return !draws.empty(); }
    Vec3f center() const { return (boundsMin + boundsMax) * 0.5f; }
    float radius() const { return (boundsMax - boundsMin).length() * 0.5f; }
};


namespace detail {

inline TextureID loadGLTFImage(VALO& valo, const GLTFImage& img, const bool srgb) {
    if (!img.path.empty()) return valo.createTexture(img.path, srgb);
    if (img.encoded.empty()) return EMPTY_TEXTURE;

    int w = 0, h = 0, channels = 0;
    stbi_uc* px = stbi_load_from_memory(
        img.encoded.data(), int(img.encoded.size()), &w, &h, &channels, 4
    );

    if (!px) {
        std::cerr << "loadModel(): embedded image: " << stbi_failure_reason() << std::endl;
        return EMPTY_TEXTURE;
    }

    const TextureID id = valo.createTexture(
        uint32_t(w), uint32_t(h), reinterpret_cast<const uint32_t*>(px), srgb
    );
    stbi_image_free(px);
    return id;
}

} // namespace detail


/*
    One image of a parsed glTF as a texture - a file beside the .gltf or bytes
    embedded in a .glb, either way. For code that works from parseGLTF()
    directly; loadModel() already does this for every material slot.

    srgb is the slot's encoding: true for base colour and emissive, false for
    the data maps (metallic-roughness, normal, occlusion), unless the shader
    decodes sRGB itself, in which case false throughout.
*/
inline TextureID loadGLTFTexture(VALO& valo, const GLTF& gltf, const int image, const bool srgb) {
    if (image < 0 || size_t(image) >= gltf.images.size()) return EMPTY_TEXTURE;
    return detail::loadGLTFImage(valo, gltf.images[size_t(image)], srgb);
}


inline Model loadModel(VALO& valo, const std::string& path) {
    Model model;

    const GLTF g = parseGLTF(path);
    if (g.vertices.empty() || g.indices.empty()) {
        std::cerr << "loadModel(): nothing to draw in \"" << path << "\"" << std::endl;
        return model;
    }

    model.verts   = valo.createVertexBuffer(g.vertices.data(), g.vertices.size(), GLTFVertex::layout());
    model.indices = valo.createIndexBuffer(g.indices.data(), g.indices.size());

    /*
        glTF fixes which slots hold colour (sRGB) and which hold data, so the
        caller never has to know. Keyed on encoding as well as image, since one
        image used as both would need two textures.
    */
    std::map<std::pair<int, bool>, TextureID> cache;

    auto texture = [&](const int image, const bool srgb) -> TextureID {
        if (image < 0 || size_t(image) >= g.images.size()) return EMPTY_TEXTURE;

        const auto key = std::make_pair(image, srgb);
        const auto it  = cache.find(key);
        if (it != cache.end()) return it->second;

        const TextureID id = detail::loadGLTFImage(valo, g.images[size_t(image)], srgb);
        if (id != EMPTY_TEXTURE) model.textures.push_back(id);
        return cache[key] = id;
    };

    for (const GLTFMaterial& m : g.materials) {
        ModelMaterial mm;
        mm.baseColor            = m.baseColor;
        mm.emissive             = m.emissive;
        mm.metallic             = m.metallic;
        mm.roughness            = m.roughness;
        mm.alphaCutoff          = m.alphaCutoff;
        mm.alphaMode            = uint32_t(m.alphaMode);
        mm.doubleSided          = m.doubleSided ? 1u : 0u;

        mm.baseColorTex         = texture(m.baseColorTex,         true);
        mm.metallicRoughnessTex = texture(m.metallicRoughnessTex, false);
        mm.normalTex            = texture(m.normalTex,            false);
        mm.occlusionTex         = texture(m.occlusionTex,         false);
        mm.emissiveTex          = texture(m.emissiveTex,          true);

        model.materials.push_back(mm);
    }

    // glTF lets a primitive have no material; it gets a plain grey one, so
    // every draw's material index can be used without checking it
    const uint32_t fallback = uint32_t(model.materials.size());
    {
        ModelMaterial grey;
        grey.baseColor = {0.8f, 0.8f, 0.8f, 1.f};
        grey.metallic  = 0.f;
        grey.roughness = 0.6f;
        model.materials.push_back(grey);
    }

    for (const GLTFNode& node : g.nodes) {
        if (node.mesh < 0 || size_t(node.mesh) >= g.meshes.size()) continue;

        // a mirroring transform turns every triangle inside out
        const bool mirrored = node.world.determinant() < 0.f;

        for (const GLTFPrimitive& prim : g.meshes[size_t(node.mesh)].primitives) {
            ModelDraw d;
            d.world    = node.world;
            d.material = prim.material >= 0 ? uint32_t(prim.material) : fallback;

            const ModelMaterial& mm = model.materials[d.material];

            d.cmd.verts      = model.verts;
            d.cmd.indices    = model.indices;
            d.cmd.firstIndex = prim.firstIndex;
            d.cmd.indexCount = prim.indexCount;
            d.cmd.cullmode   = mm.doubleSided ? CullMode::None : CullMode::Back;
            d.cmd.winding    = mirrored ? VtxWinding::CW : VtxWinding::CCW;

            /*
                Blended draws keep writing depth. Turning it off is only right
                when transparent draws are sorted back to front, which VALO does
                not do - and exporters (Sketchfab especially) mark plenty of
                opaque surfaces BLEND, which then stop hiding what is behind
                them. Ordering them after opaque draws, below, is what matters.
            */
            if (mm.alphaMode == uint32_t(AlphaMode::Blend))
                d.cmd.blendmode = BlendMode::Alpha;

            model.draws.push_back(d);
        }
    }

    // opaque first, so blended draws land on top of a finished depth buffer
    std::stable_sort(model.draws.begin(), model.draws.end(),
        [&](const ModelDraw& a, const ModelDraw& b) {
            const bool ab = model.materials[a.material].alphaMode == uint32_t(AlphaMode::Blend);
            const bool bb = model.materials[b.material].alphaMode == uint32_t(AlphaMode::Blend);
            return !ab && bb;
        });

    if (!model.draws.empty()) {
        model.boundsMin = g.bounds.min;
        model.boundsMax = g.bounds.max;
    }

    return model;
}


/*
    Every draw of a model with one program. uniformsFor is called once per draw
    and returns that draw's Uniforms struct - whatever type the program takes:

        submitModel(valo, model, program, [&](const ModelDraw& d, const ModelMaterial& m) {
            return MyUniforms{ viewProj * d.world, m };
        });
*/
template <typename UniformsFor>
void submitModel(
    VALO& valo,
    const Model& model,
    const ProgramID program,
    UniformsFor&& uniformsFor,
    const LayerID layer = VALO::BASE_LAYER
) {
    for (const ModelDraw& d : model.draws) {
        RenderCommand cmd = d.cmd;
        cmd.program = program;
        cmd.setUniforms(uniformsFor(d, model.materials[d.material]));
        valo.submit(cmd, layer);
    }
}


inline void destroyModel(VALO& valo, Model& model) {
    for (const TextureID t : model.textures) valo.destroyTexture(t);
    if (model.verts   != EMPTY_BUFFER) valo.destroyVertexBuffer(model.verts);
    if (model.indices != EMPTY_BUFFER) valo.destroyIndexBuffer(model.indices);
    model = Model{};
}

} // namespace valo
