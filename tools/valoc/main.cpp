/*
    valoc - the VALO C++ shader to GLSL translator.

        valoc <shader.hpp> [-o out.glsl] [-I dir]...

    The stage comes from the filename (.vert.hpp / .frag.hpp) rather than a
    flag, so the two can never disagree.

    Step 1 parses and reports; it emits no GLSL yet.
*/

#include <clang-c/Index.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace {

struct Field {
    std::string type;
    std::string name;
};

struct Struct {
    std::string         name;
    std::vector<Field>  fields;
};

enum class Stage { Vertex, Fragment, Unknown };


std::string str(CXString s) {
    const char* c = clang_getCString(s);
    std::string out = c ? c : "";
    clang_disposeString(s);
    return out;
}


Stage stageOf(const std::string& path) {
    if (path.find(".vert.") != std::string::npos) return Stage::Vertex;
    if (path.find(".frag.") != std::string::npos) return Stage::Fragment;
    return Stage::Unknown;
}


struct Collect {
    std::string                 entry;
    std::vector<std::string>    paramTypes;
    std::vector<std::string>    paramNames;
    std::vector<Struct>         structs;
    int                         bodyStatements = 0;
    CXCursor                    body = clang_getNullCursor();
    CXTranslationUnit           tu = nullptr;

    // everything the entry point can reach that GLSL also has to declare,
    // kept in source order so a helper is defined before it is called
    std::vector<CXCursor>       helpers;
    std::vector<CXCursor>       globals;

    // every VALO_*_SHADER in the translation unit: an included shader file
    // brings its own entry point, which is a shader rather than a helper
    std::vector<std::string>    allEntries;

    // structs written in a shader file, in source order - a constant can use
    // one the uniform block never mentions
    std::vector<std::string>    shaderStructs;

    // offset -> GLSL type, so a bare { a, b } can become vec2(a, b)
    std::vector<std::pair<unsigned, std::string>> initLists;
};


// a cursor written in a shader file, rather than pulled in from VALO or ext
bool isShaderSource(CXCursor c) {
    CXFile file = nullptr;
    unsigned line = 0, col = 0, off = 0;
    clang_getSpellingLocation(clang_getCursorLocation(c), &file, &line, &col, &off);
    if (!file) return false;

    std::string path = str(clang_getFileName(file));
    for (char& ch : path) if (ch == '\\') ch = '/';

    /*
        Only a shader file counts. Testing for library paths instead fails the
        moment clang reports a relative one, and this is the rule the design
        already asks for: a shader includes shader files and nothing else.
    */
    auto endsWith = [&](const char* suffix) {
        const std::string s(suffix);
        return path.size() >= s.size()
            && path.compare(path.size() - s.size(), s.size(), s) == 0;
    };

    return endsWith(".vert.hpp") || endsWith(".frag.hpp");
}


unsigned cursorOffset(CXCursor c) {
    CXFile file = nullptr;
    unsigned line = 0, col = 0, off = 0;
    clang_getSpellingLocation(clang_getCursorLocation(c), &file, &line, &col, &off);
    return off;
}


/*
    The entry point is found through the registration macro rather than by
    signature matching. VALO_VERTEX_SHADER(fn) expands to a variable named
    valo_reg_<fn>, so a file with no registration is an error rather than a
    guess, and a helper that happens to match the signature cannot be mistaken
    for the shader.
*/
// every registration in the TU, in order. Includes are visited first, so the
// last one is this file's own entry point and the rest are included shaders.
CXChildVisitResult visitEntry(CXCursor c, CXCursor, CXClientData data) {
    if (clang_getCursorKind(c) == CXCursor_VarDecl) {
        const std::string varName = str(clang_getCursorSpelling(c));
        if (varName.rfind("valo_reg_", 0) == 0)
            static_cast<std::vector<std::string>*>(data)->push_back(varName.substr(9));
    }
    return CXChildVisit_Recurse;
}


CXChildVisitResult visitFields(CXCursor c, CXCursor, CXClientData data) {
    if (clang_getCursorKind(c) == CXCursor_FieldDecl) {
        auto* s = static_cast<Struct*>(data);
        s->fields.push_back({
            str(clang_getTypeSpelling(clang_getCursorType(c))),
            str(clang_getCursorSpelling(c))
        });
    }
    return CXChildVisit_Continue;
}


// strips const and the reference, so "const Vertex &" and "Varyings &" both
// come back as the struct name the declaration used
std::string bareType(CXType t) {
    CXType stripped = clang_getNonReferenceType(t);
    stripped = clang_getUnqualifiedType(stripped);
    return str(clang_getTypeSpelling(stripped));
}


CXChildVisitResult countStatements(CXCursor, CXCursor, CXClientData data) {
    ++*static_cast<int*>(data);
    return CXChildVisit_Continue;
}


CXChildVisitResult visitBody(CXCursor c, CXCursor, CXClientData data) {
    auto* out = static_cast<Collect*>(data);

    if (clang_getCursorKind(c) == CXCursor_CompoundStmt) {
        out->body = c;
        clang_visitChildren(c, countStatements, &out->bodyStatements);
    }

    return CXChildVisit_Continue;
}


CXChildVisitResult visitAll(CXCursor c, CXCursor, CXClientData data) {
    auto* out = static_cast<Collect*>(data);

    if (clang_getCursorKind(c) == CXCursor_FunctionDecl
        && str(clang_getCursorSpelling(c)) == out->entry
        && clang_isCursorDefinition(c)
    ) {
        const int n = clang_Cursor_getNumArguments(c);
        for (int i = 0; i < n; ++i)
        {
            CXCursor arg = clang_Cursor_getArgument(c, i);
            out->paramTypes.push_back(bareType(clang_getCursorType(arg)));
            out->paramNames.push_back(str(clang_getCursorSpelling(arg)));
        }

        clang_visitChildren(c, visitBody, out);
    }

    /*
        Free functions and constants written in the shader file have to come
        across too - GLSL has no other way to see them, and a shader that calls
        a helper would otherwise emit a reference to nothing.
    */
    if (clang_getCursorKind(c) == CXCursor_FunctionDecl
        && clang_isCursorDefinition(c)
        && isShaderSource(c)
    ) {
        const std::string fn = str(clang_getCursorSpelling(c));

        bool isEntry = fn == out->entry;
        for (const auto& e : out->allEntries) if (fn == e) isEntry = true;

        if (!isEntry) out->helpers.push_back(c);
    }

    if (clang_getCursorKind(c) == CXCursor_VarDecl
        && isShaderSource(c)
        && str(clang_getCursorSpelling(c)).rfind("valo_reg_", 0) != 0
        && clang_getCursorSemanticParent(c).kind != CXCursor_FunctionDecl
    ) {
        out->globals.push_back(c);
    }

    // the type of a brace initialiser, which the token stream cannot recover
    /*
        A braced assignment to a class type goes through operator=, so clang
        emits no InitListExpr for it. Recording the type of whatever cursor
        begins at a given offset covers every spelling, and the token loop only
        consults this when it is actually looking at a brace.
    */
    /*
        A braced assignment to a class type goes through operator=, so clang
        wraps the list in some other expression. Recording every expression that
        begins at a brace covers each spelling, and the token loop only consults
        this when it is actually looking at one.
    */
    const CXCursorKind kind = clang_getCursorKind(c);

    if (kind == CXCursor_InitListExpr && isShaderSource(c)) {
        CXFile f = nullptr;
        unsigned l = 0, col = 0, startOff = 0;
        clang_getSpellingLocation(
            clang_getRangeStart(clang_getCursorExtent(c)), &f, &l, &col, &startOff);

        CXType ty = clang_getNonReferenceType(clang_getCursorType(c));
        const std::string spelling = str(clang_getTypeSpelling(clang_getUnqualifiedType(ty)));

        if (!spelling.empty() && spelling != "void")
            out->initLists.push_back({ startOff, spelling });
    }

    if (clang_getCursorKind(c) == CXCursor_StructDecl && clang_isCursorDefinition(c)) {
        Struct s{ str(clang_getCursorSpelling(c)), {} };
        clang_visitChildren(c, visitFields, &s);

        if (!s.name.empty()) {
            if (isShaderSource(c)) out->shaderStructs.push_back(s.name);
            out->structs.push_back(std::move(s));
        }
    }

    return CXChildVisit_Recurse;
}


const Struct* findStruct(const Collect& c, const std::string& name) {
    // the parameter type may be namespace-qualified while the decl is not
    const size_t colons = name.rfind("::");
    const std::string bare = colons == std::string::npos ? name : name.substr(colons + 2);

    for (const auto& s : c.structs) if (s.name == bare) return &s;
    return nullptr;
}


void printStruct(const char* label, const Struct* s) {
    if (!s) {
        std::printf("  %-12s <not found>\n", label);
        return;
    }

    std::printf("  %-12s %s { ", label, s->name.c_str());
    for (const auto& f : s->fields) std::printf("%s %s; ", f.type.c_str(), f.name.c_str());
    std::printf("}\n");
}


/* ========================================================================= //
    Emission
// ========================================================================= */

// VALO types map onto GLSL builtins; anything left over is a user struct and
// gets emitted as one.
std::string glslType(const std::string& cxx) {
    const size_t colons = cxx.rfind("::");
    const std::string t = colons == std::string::npos ? cxx : cxx.substr(colons + 2);

    if (t == "float")    return "float";
    if (t == "double")   return "double";
    if (t == "bool")     return "bool";
    if (t == "int"    || t == "int32_t")  return "int";
    if (t == "unsigned int" || t == "uint32_t" || t == "TextureID"
     || t == "RenderTargetID" || t == "UniformBufferID")  return "uint";

    if (t == "Vec2f") return "vec2";
    if (t == "Vec3f") return "vec3";
    if (t == "Vec4f" || t == "Color") return "vec4";
    if (t == "Vec2i") return "ivec2";
    if (t == "Vec3i") return "ivec3";
    if (t == "Vec4i") return "ivec4";
    if (t == "Vec2u") return "uvec2";
    if (t == "Vec3u") return "uvec3";
    if (t == "Vec4u") return "uvec4";
    if (t == "Mat2f") return "mat2";
    if (t == "Mat3f") return "mat3";
    if (t == "Mat4f") return "mat4";

    return t;   // a user struct, emitted separately
}


bool isBuiltinType(const std::string& glsl) {
    static const char* kBuiltins[] = {
        "float", "double", "bool", "int", "uint",
        "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4",
        "uvec2", "uvec3", "uvec4", "mat2", "mat3", "mat4"
    };
    for (const char* b : kBuiltins) if (glsl == b) return true;
    return false;
}



std::string glslField(const std::string& cxxType, const std::string& name) {
    const size_t bracket = cxxType.find('[');
    if (bracket == std::string::npos) return glslType(cxxType) + " " + name;

    return glslType(cxxType.substr(0, bracket)) + " " + name + cxxType.substr(bracket);
}

// a location holds one vec4, so a matrix spans several
int locationSpan(const std::string& glsl) {
    if (glsl == "mat4") return 4;
    if (glsl == "mat3") return 3;
    if (glsl == "mat2") return 2;
    return 1;
}


/*
    User structs nested inside the uniform block have to be declared before it,
    innermost first, and only once however many fields reference them.
*/
void emitStructs(
    std::string& out,
    const Collect& c,
    const Struct* s,
    std::vector<std::string>& done
) {
    if (!s) return;

    for (const auto& f : s->fields) {
        const std::string g = glslType(f.type);
        if (isBuiltinType(g)) continue;

        bool seen = false;
        for (const auto& d : done) if (d == g) seen = true;
        if (seen) continue;

        emitStructs(out, c, findStruct(c, g), done);

        done.push_back(g);
        const Struct* nested = findStruct(c, g);
        if (!nested) continue;

        out += "struct " + g + " {\n";
        for (const auto& nf : nested->fields)
            out += "    " + glslField(nf.type, nf.name) + ";\n";
        out += "};\n\n";
    }
}


void emitBlock(std::string& out, const Struct* s, const std::string& instance) {
    if (!s) return;

    // scalar layout, so this block matches the C++ struct field for field with
    // no padding arithmetic - which is the whole reason the feature is enabled
    out += "layout(scalar, push_constant) uniform Push {\n";
    for (const auto& f : s->fields)
        out += "    " + glslField(f.type, f.name) + ";\n";
    out += "} " + instance + ";\n\n";
}


void emitIO(
    std::string& out,
    const Struct* s,
    const char* direction,
    const std::string& prefix
) {
    if (!s) return;

    int location = 0;
    for (const auto& f : s->fields) {
        const std::string g = glslType(f.type);
        out += "layout(location = " + std::to_string(location) + ") "
             + direction + " " + g + " " + prefix + f.name + ";\n";
        location += locationSpan(g);
    }
    out += "\n";
}


/*
    The body is translated token by token, not by rebuilding the AST. The two
    languages share C syntax for everything a shader uses - the differences are
    names, brace initialisation and literal suffixes - so a rename pass over the
    token stream is both shorter and easier to check against the original than
    an expression emitter would be.
*/
struct Rename {
    std::string from;
    std::string to;
};


// GLSL keywords that are ordinary identifiers in C++, so a local named `out`
// or `in` compiles here and not there
bool isReservedInGlsl(const std::string& t) {
    static const char* kReserved[] = {
        "in", "out", "inout", "uniform", "varying", "attribute", "buffer",
        "shared", "layout", "sampler", "image", "flat", "smooth", "noperspective",
        "centroid", "patch", "sample", "subroutine", "precision", "invariant",
        "lowp", "mediump", "highp", "common", "partition", "active",
        "half", "fixed", "hvec2", "hvec3", "hvec4", "fvec2", "fvec3", "fvec4",
        "dvec2", "dvec3", "dvec4", "cast", "namespace", "using", "this",
        "filter", "input", "output"
    };
    for (const char* r : kReserved) if (t == r) return true;
    return false;
}


/*
    The one construct with no shared spelling: C++ builds a Sampler value at the
    call site, GLSL indexes a fixed array of them. The arguments are compile-time
    enumerators either way, so the call collapses to that index.
*/
std::string samplerConstant(const std::vector<std::string>& args) {
    auto wrap = [](const std::string& a) -> uint32_t {
        if (a == "Clamp")  return 1;
        if (a == "Mirror") return 2;
        return 0;   // Repeat, and the default when the argument is omitted
    };

    const uint32_t u = args.size() > 0 ? wrap(args[0]) : 0;
    const uint32_t v = args.size() > 1 ? wrap(args[1]) : 0;
    const uint32_t f = (args.size() > 2 && args[2] == "Nearest") ? 0 : 1;

    // valo.glsl names the symmetric pairs; anything else has no name to use
    if (u == v) {
        static const char* kWrap[] = { "REPEAT", "CLAMP", "MIRROR" };
        return std::string("VALO_") + kWrap[u] + (f ? "_LINEAR" : "_NEAREST");
    }

    return std::to_string((u * 3 + v) * 2 + f) + "u";
}


std::string stripFloatSuffix(const std::string& lit) {
    if (lit.size() < 2) return lit;

    const char back = lit.back();
    if (back != 'f' && back != 'F') return lit;

    std::string n = lit.substr(0, lit.size() - 1);

    // GLSL has no trailing-dot form, so 1.f becomes 1.0 and not 1.
    if (!n.empty() && n.back() == '.') return n + "0";

    if (n.find('.') == std::string::npos && n.find('e') == std::string::npos
        && n.find('E') == std::string::npos) n += ".0";

    return n;
}


/*
    VALO spells vector maths as methods and GLSL as free functions, so
    a.normalized() has to become normalize(a). The receiver has already been
    emitted by the time the method name shows up, so it is lifted back out of
    the buffer and re-emitted inside the call.
*/
const char* glslMethod(const std::string& name) {
    if (name == "normalized") return "normalize";
    if (name == "length")     return "length";
    if (name == "dot")        return "dot";
    if (name == "cross")      return "cross";
    if (name == "reflect")    return "reflect";
    if (name == "refract")    return "refract";
    if (name == "distance")   return "distance";
    return nullptr;
}


// pulls the trailing expression off the buffer: a balanced (...) or [...] run
// followed by identifier characters and member accesses
std::string takeReceiver(std::string& out) {
    while (!out.empty() && out.back() == ' ') out.pop_back();

    size_t end = out.size();
    size_t i = end;

    while (i > 0) {
        const char ch = out[i - 1];

        if (ch == ')' || ch == ']') {
            const char open = ch == ')' ? '(' : '[';
            int depth = 0;
            while (i > 0) {
                const char d = out[i - 1];
                if (d == ch)   ++depth;
                if (d == open) --depth;
                --i;
                if (depth == 0) break;
            }
            continue;
        }

        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '.') {
            --i;
            continue;
        }

        break;
    }

    std::string receiver = out.substr(i, end - i);
    out.erase(i);
    return receiver;
}


std::string translateRange(
    const Collect& c,
    CXCursor cursor,
    const std::vector<Rename>& renames,
    bool dropOuterBraces
) {
    if (clang_Cursor_isNull(cursor)) return "    // not found\n";

    CXSourceRange range = clang_getCursorExtent(cursor);

    CXToken* tokens = nullptr;
    unsigned count = 0;
    clang_tokenize(c.tu, range, &tokens, &count);

    std::string out;
    std::vector<bool> braceIsCall;      // one entry per open brace still unclosed

    int  depth      = 1;
    int  parenDepth = 0;
    bool lineStart  = true;

    auto indent = [&]() {
        for (int i = 0; i < depth; ++i) out += "    ";
    };

    auto newline = [&]() {
        while (!out.empty() && (out.back() == ' ')) out.pop_back();
        if (!out.empty() && out.back() == '\n') { lineStart = true; return; }
        out += "\n";
        lineStart = true;
    };

    auto put = [&](const std::string& text, bool trailingSpace = true) {
        if (lineStart) { indent(); lineStart = false; }
        out += text;
        if (trailingSpace) out += " ";
    };

    auto tight = [&](const std::string& text, bool trailingSpace) {
        if (lineStart) { indent(); lineStart = false; }
        else while (!out.empty() && out.back() == ' ') out.pop_back();
        out += text;
        if (trailingSpace) out += " ";
    };

    std::string prevSpelling;

    for (unsigned i = 0; i < count; ++i) {
        const CXTokenKind kind = clang_getTokenKind(tokens[i]);

        // comments carry no meaning across and a // would swallow the rest of
        // the emitted line, since the token stream has no newlines in it
        if (kind == CXToken_Comment) continue;

        std::string t = str(clang_getTokenSpelling(c.tu, tokens[i]));

        // for a function body the outermost braces are emitted by the caller
        if (dropOuterBraces && i == 0 && t == "{") continue;
        if (dropOuterBraces && i + 1 == count && t == "}") continue;

        /*
            "using namespace valo;" has no GLSL equivalent and the qualifier it
            exists to avoid is stripped anyway, so the whole statement goes.
        */
        if (kind == CXToken_Keyword && t == "using") {
            while (i < count && str(clang_getTokenSpelling(c.tu, tokens[i])) != ";") ++i;
            continue;
        }

        if (kind == CXToken_Literal) {
            put(stripFloatSuffix(t));
            prevSpelling = t;
            continue;
        }

        if (kind == CXToken_Identifier) {
            // Sampler(...) collapses to its index in uSamplers
            if (t == "Sampler" && i + 1 < count
                && str(clang_getTokenSpelling(c.tu, tokens[i + 1])) == "("
            ) {
                /*
                    Only the enumerator after each :: is an argument. Collecting
                    every token instead picks up the enum type names too, which
                    shifts every index by one and silently resolves every call
                    to the same sampler.
                */
                std::vector<std::string> args;
                int parens = 0;
                unsigned j = i + 1;
                bool afterScope = false;

                for (; j < count; ++j) {
                    const std::string a = str(clang_getTokenSpelling(c.tu, tokens[j]));
                    if (a == "(") { ++parens; continue; }
                    if (a == ")") { if (--parens == 0) break; continue; }
                    if (a == "::") { afterScope = true; continue; }

                    if (afterScope) { args.push_back(a); afterScope = false; }
                }

                put(samplerConstant(args));
                i = j;
                prevSpelling = ")";
                continue;
            }

            /*
                GLSL has no namespaces, so a qualifier is dropped along with its
                ::. That also maps std::max onto GLSL's max for free, and leaves
                valo::Vec2f to be renamed as a bare Vec2f below.
            */
            if (i + 1 < count
                && str(clang_getTokenSpelling(c.tu, tokens[i + 1])) == "::"
            ) {
                ++i;
                continue;
            }

            // a variable template: emit the value and swallow <float>
            if (t == "pi_v") {
                put("3.14159265358979");
                if (i + 1 < count && str(clang_getTokenSpelling(c.tu, tokens[i + 1])) == "<")
                    while (i < count && str(clang_getTokenSpelling(c.tu, tokens[i])) != ">") ++i;
                prevSpelling = "3.14159265358979";
                continue;
            }

            bool renamed = false;
            for (const auto& r : renames) {
                if (t == r.from) { t = r.to; renamed = true; break; }
            }

            const bool ioRename = renamed;
            if (!renamed && isReservedInGlsl(t)) { t += "_"; renamed = true; }

            if (!renamed) {
                const std::string g = glslType(t);
                if (g != t) t = g;
            }

            /*
                "in" renames to "a_", so the following ".uv" has to be absorbed
                rather than emitted - the struct became loose variables and
                there is nothing left to take a member of.
            */
            if (ioRename && !t.empty() && t.back() == '_'
                && i + 1 < count
                && str(clang_getTokenSpelling(c.tu, tokens[i + 1])) == "."
            ) {
                put(t, false);
                ++i;
                prevSpelling = t;
                continue;
            }

            // a type name followed by { is brace initialisation, which GLSL
            // spells as a constructor call
            if (isBuiltinType(t) && i + 1 < count
                && str(clang_getTokenSpelling(c.tu, tokens[i + 1])) == "{"
            ) {
                put(t, false);
                out += "(";
                braceIsCall.push_back(true);
                ++i;
                prevSpelling = "(";
                continue;
            }

            put(t);
            prevSpelling = t;
            continue;
        }

        if (t == "{") {
            /*
                An initialiser list carries no type in the token stream, so the
                type clang recorded for this offset supplies the constructor.
            */
            CXSourceLocation loc = clang_getTokenLocation(c.tu, tokens[i]);
            CXFile lf = nullptr; unsigned ll = 0, lc = 0, lo = 0;
            clang_getSpellingLocation(loc, &lf, &ll, &lc, &lo);

            std::string listType;
            for (const auto& [off, type] : c.initLists)
                if (off == lo) { listType = glslType(type); break; }

            /*
                An assignment of a braced list is wrapped in an operator call,
                so the recorded InitListExpr offsets can miss it. Asking what
                cursor sits on the brace works whatever node kind clang chose.
            */
            if (listType.empty() && prevSpelling == "=") {
                std::string tail = out;
                while (!tail.empty() && (tail.back() == ' ' || tail.back() == '=')) tail.pop_back();

                auto endsWith = [&](const char* w) {
                    const std::string n(w);
                    return tail.size() >= n.size()
                        && tail.compare(tail.size() - n.size(), n.size(), n) == 0;
                };

                if (endsWith("gl_Position") || endsWith("outColor")) listType = "vec4";
                else if (endsWith("gl_FragDepth") || endsWith("gl_PointSize")) listType = "float";
            }

            /*
                A brace nested in a call argument has no InitListExpr behind it
                either, and the element count is the only thing left to go on.
                Two to four elements is a vector literal; a wrong guess fails at
                glslc rather than silently.
            */
            if (listType.empty()
                && (prevSpelling == "(" || prevSpelling == "," || prevSpelling == "return")
            ) {
                int braces = 0, commas = 0;
                for (unsigned k = i; k < count; ++k) {
                    const std::string a = str(clang_getTokenSpelling(c.tu, tokens[k]));
                    if (a == "{" || a == "(" || a == "[") ++braces;
                    if (a == "}" || a == ")" || a == "]") { if (--braces == 0) break; }
                    if (a == "," && braces == 1) ++commas;
                }

                const int n = commas + 1;
                if (n >= 2 && n <= 4) listType = "vec" + std::to_string(n);
            }

            if (listType.empty() && std::getenv("VALOC_DEBUG"))
                std::fprintf(stderr, "valoc: no init-list type at offset %u\n", lo);

            if (!listType.empty() && isBuiltinType(listType)) {
                put(listType, false);
                out += "(";
                braceIsCall.push_back(true);
                prevSpelling = "(";
                continue;
            }

            braceIsCall.push_back(false);
            tight(" {", false);
            ++depth;
            newline();
            prevSpelling = t;
            continue;
        }

        if (t == "}") {
            const bool call = !braceIsCall.empty() && braceIsCall.back();
            if (!braceIsCall.empty()) braceIsCall.pop_back();

            if (call) { tight(")", true); }
            else {
                if (depth > 1) --depth;
                newline();
                put("}", false);

                // "} else {" rather than the brace stranded on its own line
                const bool elseNext = i + 1 < count
                    && str(clang_getTokenSpelling(c.tu, tokens[i + 1])) == "else";

                if (elseNext) out += " ";
                else          newline();
            }

            prevSpelling = t;
            continue;
        }

        // a ; inside parentheses is a for-header separator, not a statement end
        if (t == ";") {
            tight(";", parenDepth > 0);
            if (parenDepth == 0) newline();
            prevSpelling = t;
            continue;
        }

        // postfix sits against the operand it follows; prefix against the one
        // it precedes, so only the first should eat the preceding space
        if (t == "++" || t == "--") {
            const bool postfix = !prevSpelling.empty()
                && (std::isalnum(static_cast<unsigned char>(prevSpelling[0]))
                    || prevSpelling[0] == '_'
                    || prevSpelling == ")"
                    || prevSpelling == "]");

            if (postfix) tight(t, false);
            else         put(t, false);

            prevSpelling = t;
            continue;
        }

        /*
            Only a call tightens against its paren. Anything else - an operator,
            a keyword, the start of a line - keeps the space, so "= (" does not
            come out as "=(".
        */
        if (t == "(") {
            const bool keyword =
                prevSpelling == "if"     || prevSpelling == "for"
             || prevSpelling == "while"  || prevSpelling == "switch"
             || prevSpelling == "return" || prevSpelling == "else";

            const bool callable = !prevSpelling.empty()
                && !keyword
                && (std::isalnum(static_cast<unsigned char>(prevSpelling[0]))
                    || prevSpelling[0] == '_'
                    || prevSpelling == ")"
                    || prevSpelling == "]");

            if (callable) tight("(", false);
            else          put("(", false);

            ++parenDepth;
            prevSpelling = t;
            continue;
        }

        if (t == ".") {
            // .method( where GLSL has a free function of the same meaning
            if (i + 2 < count) {
                const std::string m = str(clang_getTokenSpelling(c.tu, tokens[i + 1]));
                const std::string paren = str(clang_getTokenSpelling(c.tu, tokens[i + 2]));

                if (paren == "(") {
                    if (const char* fn = glslMethod(m)) {
                        const std::string receiver = takeReceiver(out);
                        const bool noArgs = i + 3 < count
                            && str(clang_getTokenSpelling(c.tu, tokens[i + 3])) == ")";

                        if (lineStart) { indent(); lineStart = false; }
                        out += std::string(fn) + "(" + receiver;
                        if (!noArgs) out += ", ";

                        i += 2;             // the method name and its (
                        ++parenDepth;
                        prevSpelling = "(";
                        continue;
                    }
                }
            }

            tight(t, false);
            prevSpelling = t;
            continue;
        }

        if (t == "[") { tight(t, false); prevSpelling = t; continue; }
        if (t == ")") { if (parenDepth) --parenDepth; tight(t, true); prevSpelling = t; continue; }
        if (t == "]") { tight(t, true);  prevSpelling = t; continue; }
        if (t == ",")             { tight(",", true); prevSpelling = t; continue; }

        put(t);
        prevSpelling = t;
    }

    clang_disposeTokens(c.tu, tokens, count);

    while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
    return out.empty() ? "" : out + "\n";
}


/*
    Every generated shader includes valo.glsl, so valoc carries a copy and drops
    one beside the output when the directory has none. Embedded rather than
    copied from a known path, so the tool works from any working directory and a
    generated shader is never missing its prelude.

    An existing file is left alone - a project is free to extend it.
*/
const char* kPrelude =
R"GLSL(#extension GL_EXT_nonuniform_qualifier  : require

// lets a uniform/push-constant block mirror a C++ struct field for field,
// instead of hand-flattening around std140's padding rules
#extension GL_EXT_scalar_block_layout   : require

// matches valo::EMPTY_TEXTURE
const uint VALO_EMPTY_TEXTURE  = 0xFFFFFFFFu;

// index = (wrapU * 3 + wrapV) * 2 + filter, matching valo::samplerIndex.
// Only the symmetric pairs get names; a shader wanting different wraps per
// axis gets the raw index.
const uint VALO_REPEAT_NEAREST = 0u;
const uint VALO_REPEAT_LINEAR  = 1u;
const uint VALO_CLAMP_NEAREST  = 8u;
const uint VALO_CLAMP_LINEAR   = 9u;
const uint VALO_MIRROR_NEAREST = 16u;
const uint VALO_MIRROR_LINEAR  = 17u;

layout(set = 1, binding = 0) uniform texture2D uTextures[];
layout(set = 1, binding = 1) uniform sampler   uSamplers[];

vec4 valoTexture(uint tex, vec2 uv, uint samp) {
    return texture(sampler2D(uTextures[nonuniformEXT(tex)], uSamplers[samp]), uv);
}

// matches texture(id, uv) in C++, where the Sampler argument defaults
vec4 valoTexture(uint tex, vec2 uv) {
    return valoTexture(tex, uv, VALO_REPEAT_LINEAR);
}
)GLSL";


void writePreludeIfMissing(const std::string& outPath) {
    namespace fs = std::filesystem;

    const fs::path dir = fs::path(outPath).parent_path();
    const fs::path prelude = (dir.empty() ? fs::path(".") : dir) / "valo.glsl";

    if (fs::exists(prelude)) return;

    std::FILE* f = std::fopen(prelude.string().c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "valoc: cannot write %s\n", prelude.string().c_str());
        return;
    }

    std::fwrite(kPrelude, 1, std::strlen(kPrelude), f);
    std::fclose(f);
    std::printf("  %-12s %s\n", "prelude", prelude.string().c_str());
}


/*
    Free functions and constants, emitted ahead of main() in the order they were
    written - which is also a valid declaration order, because C++ required it.
*/

/*
    GLSL spells an array constant differently from C++:

        const Light kLights[4] = Light[4]( Light(...), Light(...), ... );

    so the braces become constructor calls rather than lists. The initialiser
    only holds literals, identifiers and nested braces, which is a small enough
    grammar to walk on its own.
*/
std::string translateArrayInit(
    const Collect& c,
    CXCursor g,
    const std::vector<Rename>& renames,
    const std::string& elemType
) {
    CXToken* tokens = nullptr;
    unsigned count = 0;
    clang_tokenize(c.tu, clang_getCursorExtent(g), &tokens, &count);

    std::string out;
    int depth = 0;
    bool started = false;

    for (unsigned i = 0; i < count; ++i) {
        const CXTokenKind kind = clang_getTokenKind(tokens[i]);
        if (kind == CXToken_Comment) continue;

        std::string t = str(clang_getTokenSpelling(c.tu, tokens[i]));

        if (!started) {
            if (t == "{") { started = true; depth = 1; }
            continue;
        }

        if (t == "{") {
            ++depth;
            // one level in is an element; deeper is a member like a vec3
            out += (depth == 2 ? elemType : std::string("vec3")) + "(";
            continue;
        }

        if (t == "}") {
            --depth;
            if (depth == 0) break;
            out += ")";
            continue;
        }

        if (kind == CXToken_Literal) { out += stripFloatSuffix(t); continue; }

        if (kind == CXToken_Identifier) {
            if (i + 1 < count && str(clang_getTokenSpelling(c.tu, tokens[i + 1])) == "::") {
                ++i;
                continue;
            }

            for (const auto& r : renames) if (t == r.from) { t = r.to; break; }
            const std::string gl = glslType(t);
            out += (gl != t ? gl : t);
            continue;
        }

        if (t == ",") { out += ", "; continue; }
        out += t;
    }

    clang_disposeTokens(c.tu, tokens, count);
    return out;
}

/*
    Constants and functions, interleaved in the order they were written. Keeping
    them in two passes puts every constant first, and a constant whose value
    calls a helper then references something GLSL has not seen yet.
*/
std::string emitHelpers(const Collect& c, const std::vector<Rename>& renames) {
    std::string out;
    std::vector<std::string> emitted;

    struct Item { unsigned off; bool isFunction; CXCursor cursor; };
    std::vector<Item> items;

    for (CXCursor g : c.globals) items.push_back({ cursorOffset(g), false, g });
    for (CXCursor h : c.helpers) items.push_back({ cursorOffset(h), true,  h });

    std::sort(items.begin(), items.end(),
        [](const Item& a, const Item& b) { return a.off < b.off; });

    for (const Item& item : items) {
        if (!item.isFunction) {
            CXCursor g = item.cursor;

            const std::string name = str(clang_getCursorSpelling(g));

            // "constexpr int a = 1, b = 2;" is several VarDecls sharing one extent,
            // so without this each re-emits the whole declaration
            bool seen = false;
            for (const auto& e : emitted) if (e == name) seen = true;
            if (seen) continue;
            emitted.push_back(name);
            // an array constant needs GLSL's own array spelling
            CXType vt = clang_getCursorType(g);
            if (vt.kind == CXType_ConstantArray) {
                CXType et = clang_getUnqualifiedType(clang_getArrayElementType(vt));
                const std::string elem = glslType(str(clang_getTypeSpelling(et)));
                const long long n = clang_getArraySize(vt);

                const std::string ns = std::to_string(n);

                std::string body = translateArrayInit(c, g, renames, elem);
                while (!body.empty() && (body.back() == ' ' || body.back() == ',')) body.pop_back();

                out += "const " + elem + " " + name + "[" + ns + "] = "
                     + elem + "[" + ns + "](" + body + ");\n";
                continue;
            }

            std::string raw = str(clang_getTypeSpelling(vt));
            if (raw.rfind("const ", 0) == 0) raw = raw.substr(6);
            const std::string type = glslType(raw);

            /*
                A scalar constexpr is folded rather than translated, so things
                with no GLSL spelling - sizeof, integer division, casts - come
                out as the value the C++ already computed.
            */
            if (type == "int" || type == "uint" || type == "float") {
                CXEvalResult ev = clang_Cursor_Evaluate(g);
                if (ev) {
                    const CXEvalResultKind ek = clang_EvalResult_getKind(ev);
                    char buf[64] = {0};

                    if (ek == CXEval_Int)
                        std::snprintf(buf, sizeof buf, "%lld",
                            (long long)clang_EvalResult_getAsLongLong(ev));
                    else if (ek == CXEval_Float)
                        std::snprintf(buf, sizeof buf, "%.9g",
                            clang_EvalResult_getAsDouble(ev));

                    if (buf[0]) {
                        std::string lit = buf;
                        if (type == "float" && lit.find('.') == std::string::npos
                            && lit.find('e') == std::string::npos) lit += ".0";

                        out += "const " + type + " " + name + " = " + lit + ";\n";
                        clang_EvalResult_dispose(ev);
                        continue;
                    }

                    clang_EvalResult_dispose(ev);
                }
            }

            std::string init = translateRange(c, g, renames, false);
            while (!init.empty() && (init.back() == '\n' || init.back() == ' ')) init.pop_back();

            // the declaration is re-spelled; only the initialiser is carried over
            const size_t eq = init.find('=');
            if (eq == std::string::npos) continue;

            // and the initialiser stops at the comma that begins the next one
            std::string value = init.substr(eq + 1);
            int vdepth = 0;
            for (size_t k = 0; k < value.size(); ++k) {
                const char ch = value[k];
                if (ch == '(' || ch == '[' || ch == '{') ++vdepth;
                if (ch == ')' || ch == ']' || ch == '}') --vdepth;
                if (ch == ',' && vdepth == 0) { value = value.substr(0, k); break; }
            }

            out += "const " + type + " " + name + " =" + value + ";\n";
            continue;
        }

        CXCursor h = item.cursor;

        const CXType fn = clang_getCursorType(h);
        const std::string ret = glslType(str(clang_getTypeSpelling(clang_getResultType(fn))));

        out += ret + " " + str(clang_getCursorSpelling(h)) + "(";

        const int n = clang_Cursor_getNumArguments(h);
        for (int i = 0; i < n; ++i) {
            CXCursor arg = clang_Cursor_getArgument(h, i);
            if (i) out += ", ";
            out += glslType(bareType(clang_getCursorType(arg))) + " "
                 + str(clang_getCursorSpelling(arg));
        }

        out += ") {\n";

        CXCursor body = clang_getNullCursor();
        clang_visitChildren(h, [](CXCursor k, CXCursor, CXClientData d) {
            if (clang_getCursorKind(k) == CXCursor_CompoundStmt)
                *static_cast<CXCursor*>(d) = k;
            return CXChildVisit_Continue;
        }, &body);

        out += translateRange(c, body, renames, true);
        out += "}\n\n";
    }

    return out;
}


std::string emit(const Collect& c, Stage stage) {
    const Struct* a = findStruct(c, c.paramTypes[0]);
    const Struct* b = findStruct(c, c.paramTypes[1]);
    const Struct* v = stage == Stage::Vertex ? findStruct(c, c.paramTypes[2]) : a;
    const Struct* uniforms = stage == Stage::Vertex ? b : b;

    std::string out =
        "#version 450\n"
        "#extension GL_EXT_scalar_block_layout : require\n\n"
        "// generated by valoc - do not edit\n\n"
        "#include \"valo.glsl\"\n\n";

    std::vector<std::string> done;
    emitStructs(out, c, uniforms, done);

    /*
        Then every other struct the shader file defines. Walking out from the
        uniform block alone misses the ones only a constant or a helper uses,
        and the three parameter structs are already in/out declarations.
    */
    for (const std::string& name : c.shaderStructs) {
        if (name == c.paramTypes[0] || name == c.paramTypes[1]) continue;
        if (stage == Stage::Vertex && name == c.paramTypes[2]) continue;

        bool seen = false;
        for (const auto& d : done) if (d == name) seen = true;
        if (seen) continue;

        const Struct* st = findStruct(c, name);
        if (!st) continue;

        done.push_back(name);
        out += "struct " + name + " {\n";
        for (const auto& f : st->fields)
            out += "    " + glslField(f.type, f.name) + ";\n";
        out += "};\n\n";
    }

    if (stage == Stage::Vertex) {
        emitIO(out, a, "in",  "a_");
        emitIO(out, v, "out", "v_");
    } else {
        emitIO(out, v, "in", "v_");
        out += "layout(location = 0) out vec4 outColor;\n\n";
    }

    // the block instance takes the name of the C++ parameter, so u.mvp needs no
    // renaming when the body is translated
    emitBlock(out, uniforms, c.paramNames[stage == Stage::Vertex ? 1 : 1]);

    /*
        in./out./uniform member access is rewritten by renaming the parameter
        itself: "in" becomes "a_" so "in.pos" falls out as "a_pos". The uniform
        parameter keeps its own name, which is why the block was named after it.
    */
    std::vector<Rename> renames;

    if (stage == Stage::Vertex) {
        renames.push_back({ c.paramNames[0], "a_" });
        renames.push_back({ c.paramNames[2], "v_" });
    } else {
        renames.push_back({ c.paramNames[0], "v_" });
    }

    renames.push_back({ "UINT32_MAX",     "0xFFFFFFFFu" });
    renames.push_back({ "lerp",           "mix" });
    renames.push_back({ "texture",        "valoTexture" });
    renames.push_back({ "EMPTY_TEXTURE",  "VALO_EMPTY_TEXTURE" });
    renames.push_back({ "valo_Position",  "gl_Position" });
    renames.push_back({ "valo_FragColor", "outColor" });
    renames.push_back({ "valo_FragDepth", "gl_FragDepth" });
    renames.push_back({ "valo_FragCoord", "gl_FragCoord" });
    renames.push_back({ "valo_VertexIndex",   "gl_VertexIndex" });
    renames.push_back({ "valo_InstanceIndex", "gl_InstanceIndex" });
    renames.push_back({ "valo_FrontFacing",   "gl_FrontFacing" });
    renames.push_back({ "valo_PointSize",     "gl_PointSize" });
    renames.push_back({ "valo_PointCoord",    "gl_PointCoord" });

    out += emitHelpers(c, renames);

    out += "void main() {\n";
    out += translateRange(c, c.body, renames, true);
    out += "}\n";
    return out;
}

} // namespace


int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: valoc <shader.hpp> [-o out.glsl] [-I dir]...\n");
        return 2;
    }

    const std::string input = argv[1];
    std::string output;
    std::vector<std::string> includes;

    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-o") && i + 1 < argc)      output = argv[++i];
        else if (!std::strcmp(argv[i], "-I") && i + 1 < argc) includes.push_back(argv[++i]);
        else if (!std::strncmp(argv[i], "-I", 2))             includes.push_back(argv[i] + 2);
    }

    const Stage stage = stageOf(input);
    if (stage == Stage::Unknown) {
        std::fprintf(stderr, "valoc: %s: name must contain .vert. or .frag.\n", input.c_str());
        return 2;
    }

    std::vector<std::string> argStrings{ "-std=c++20", "-xc++" };

#ifdef VALOC_CLANG_RESOURCE
    // not a normal include path, so it is baked in at configure time
    argStrings.push_back(std::string("-I") + VALOC_CLANG_RESOURCE);
#endif

    for (const auto& inc : includes) argStrings.push_back("-I" + inc);

    std::vector<const char*> args;
    for (const auto& a : argStrings) args.push_back(a.c_str());

    CXIndex index = clang_createIndex(0, 0);
    CXTranslationUnit tu = nullptr;

    const CXErrorCode err = clang_parseTranslationUnit2(
        index, input.c_str(), args.data(), int(args.size()),
        nullptr, 0, CXTranslationUnit_None, &tu
    );

    if (err != CXError_Success || !tu) {
        std::fprintf(stderr, "valoc: %s: parse failed (%d)\n", input.c_str(), int(err));
        clang_disposeIndex(index);
        return 1;
    }

    /*
        A half-parsed AST still walks cleanly and would produce plausible but
        wrong GLSL, so any error-severity diagnostic is fatal here.
    */
    unsigned fatal = 0;
    const unsigned diagnostics = clang_getNumDiagnostics(tu);

    for (unsigned i = 0; i < diagnostics; ++i) {
        CXDiagnostic d = clang_getDiagnostic(tu, i);

        if (clang_getDiagnosticSeverity(d) >= CXDiagnostic_Error) {
            ++fatal;
            std::fprintf(stderr, "valoc: %s\n",
                str(clang_formatDiagnostic(d, clang_defaultDiagnosticDisplayOptions())).c_str());
        }

        clang_disposeDiagnostic(d);
    }

    int rc = 0;

    if (fatal) {
        std::fprintf(stderr, "valoc: %s: %u error(s), refusing to translate\n", input.c_str(), fatal);
        rc = 1;
    } else {
        CXCursor root = clang_getTranslationUnitCursor(tu);

        std::vector<std::string> entries;
        clang_visitChildren(root, visitEntry, &entries);

        const std::string entry = entries.empty() ? std::string() : entries.back();

        if (entry.empty()) {
            std::fprintf(stderr,
                "valoc: %s: no VALO_VERTEX_SHADER / VALO_FRAGMENT_SHADER registration\n",
                input.c_str());
            rc = 1;
        } else {
            Collect collected;
            collected.entry      = entry;
            collected.tu         = tu;
            collected.allEntries = entries;
            clang_visitChildren(root, visitAll, &collected);

            const size_t expected = stage == Stage::Vertex ? 3 : 2;

            std::printf("%s\n", input.c_str());
            std::printf("  %-12s %s\n", "stage", stage == Stage::Vertex ? "vertex" : "fragment");
            std::printf("  %-12s %s\n", "entry", collected.entry.c_str());

            if (collected.paramTypes.size() != expected) {
                std::fprintf(stderr, "valoc: %s: %s has %zu parameters, expected %zu\n",
                    input.c_str(), collected.entry.c_str(), collected.paramTypes.size(), expected);
                rc = 1;
            } else if (stage == Stage::Vertex) {
                printStruct("vertex in", findStruct(collected, collected.paramTypes[0]));
                printStruct("uniforms",  findStruct(collected, collected.paramTypes[1]));
                printStruct("varyings",  findStruct(collected, collected.paramTypes[2]));
            } else {
                printStruct("varyings", findStruct(collected, collected.paramTypes[0]));
                printStruct("uniforms", findStruct(collected, collected.paramTypes[1]));
            }

            std::printf("  %-12s %d statements\n", "body", collected.bodyStatements);

            if (rc == 0) {
                const std::string glsl = emit(collected, stage);

                if (output.empty()) {
                    std::printf("\n%s", glsl.c_str());
                } else if (std::FILE* f = std::fopen(output.c_str(), "wb")) {
                    std::fwrite(glsl.data(), 1, glsl.size(), f);
                    std::fclose(f);
                    std::printf("  %-12s %s\n", "wrote", output.c_str());
                    writePreludeIfMissing(output);
                } else {
                    std::fprintf(stderr, "valoc: cannot write %s\n", output.c_str());
                    rc = 1;
                }
            }
        }
    }

    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);
    return rc;
}
