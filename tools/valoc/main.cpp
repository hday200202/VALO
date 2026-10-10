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
#include <fstream>
#include <string>
#include <vector>

namespace {

struct Field {
    std::string type;
    std::string name;
    std::string init;       // the default member initialiser, already GLSL; empty if none
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
    std::string                 entryKey;       // startKey of the entry function
    CXCursor                    entryFn = clang_getNullCursor();
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

    /*
        `auto f = [](float v) { ... };` inside a helper. GLSL has no lambdas, so
        each becomes a top-level function named after the variable - which means
        call sites, already spelling that name, need no rewriting at all. The
        offset recorded is the enclosing function's, so it is emitted just
        before the code that uses it.
    */
    struct Lambda { std::string name; CXCursor expr; std::string hostKey; };
    std::vector<Lambda> lambdas;

    /*
        startKey -> the order visitAll met each function and constant in. The
        translation unit is walked in preprocessed order, so an included file's
        declarations sit where its #include was - the order GLSL needs. A byte
        offset cannot say that: it restarts at 0 in every file.
    */
    std::vector<std::pair<std::string, unsigned>> declOrder;
    unsigned nextOrder = 1;

    /*
        `const Sampler samp;` declared once and reused. GLSL has no such object,
        only the index valoTexture takes, so the declaration goes and the name
        becomes that constant.
    */
    std::vector<std::string> samplerLocals;

    // name -> element count, so a range-for can become an indexed loop
    std::vector<std::pair<std::string, long long>> arrayLengths;

    // VALO_GLSL(fn, "...") - hand-written GLSL to emit in place of fn's body
    std::vector<std::pair<std::string, std::string>> glslOverrides;

    // uniformBuffer<T>(slot) - struct name and binding, one block per type
    std::vector<std::pair<std::string, long long>> uniformBlocks;

    // `auto name = ...` that is not a lambda: location of the name -> GLSL type
    std::vector<std::pair<std::string, std::string>> autoTypes;

    // a use of an enum constant: location of the name -> its value as a literal
    std::vector<std::pair<std::string, std::string>> enumValues;

    /*
        Storage slots share set 0 with the uniform slots, after them, because a
        pipeline layout may hold only one push-descriptor set. Read from the
        MAX_UNIFORM_SLOTS the shader was compiled against, so the two can never
        disagree.
    */
    unsigned storageBindingBase = 0;

    // valoc's own refusals. Any one is fatal, the same as a clang error, since
    // each of these translates to GLSL that compiles and is silently wrong.
    std::vector<std::string> errors;
};


/*
    The directory VALO's own headers live in, found from wherever
    <render/ShaderRegistry.hpp> resolved. Anything under it is library code,
    never translated.
*/
std::string g_libraryRoot;

std::string cursorPath(CXCursor c) {
    CXFile file = nullptr;
    unsigned line = 0, col = 0, off = 0;
    clang_getSpellingLocation(clang_getCursorLocation(c), &file, &line, &col, &off);
    if (!file) return {};

    std::string path = str(clang_getFileName(file));
    for (char& ch : path) if (ch == '\\') ch = '/';
    return path;
}

bool hasShaderSuffix(const std::string& path) {
    auto endsWith = [&](const char* suffix) {
        const std::string s(suffix);
        return path.size() >= s.size()
            && path.compare(path.size() - s.size(), s.size(), s) == 0;
    };
    return endsWith(".vert.hpp") || endsWith(".frag.hpp");
}

// a cursor in a shader file proper: every helper in one is translated
bool isShaderFile(CXCursor c) {
    return hasShaderSuffix(cursorPath(c));
}

/*
    A cursor written by the project rather than pulled in from VALO, ext/ or
    the system. Shader files always qualify; any other project header does
    too, but its functions and constants are emitted only when the shader
    actually reaches them (see pruneUnreached), so a shared common.hpp can
    hold helpers without every unrelated line of C++ in it being translated.
*/
bool isShaderSource(CXCursor c) {
    const std::string path = cursorPath(c);
    if (path.empty()) return false;
    if (hasShaderSuffix(path)) return true;

    if (clang_Location_isInSystemHeader(clang_getCursorLocation(c))) return false;
    if (path.rfind("ext/", 0) == 0 || path.find("/ext/") != std::string::npos) return false;
    if (!g_libraryRoot.empty() && path.rfind(g_libraryRoot, 0) == 0) return false;

    // without a library root nothing can be told apart, so keep the old rule
    return !g_libraryRoot.empty();
}


// file + offset, so a location in a helper from the .vert.hpp and one in the
// .frag.hpp body can never collide the way bare offsets can
std::string locationKey(CXSourceLocation loc) {
    CXFile file = nullptr;
    unsigned line = 0, col = 0, off = 0;
    clang_getSpellingLocation(loc, &file, &line, &col, &off);
    return (file ? str(clang_getFileName(file)) : std::string()) + ":" + std::to_string(off);
}

std::string startKey(CXCursor c) {
    return locationKey(clang_getRangeStart(clang_getCursorExtent(c)));
}

std::string tokenKey(CXTranslationUnit tu, CXToken t) {
    return locationKey(clang_getTokenLocation(tu, t));
}

// "file.hpp:12:5", for pointing an error at the line the user wrote
std::string where(CXCursor c) {
    CXFile file = nullptr;
    unsigned line = 0, col = 0, off = 0;
    clang_getSpellingLocation(clang_getCursorLocation(c), &file, &line, &col, &off);
    return (file ? str(clang_getFileName(file)) : std::string("?"))
         + ":" + std::to_string(line) + ":" + std::to_string(col);
}


/*
    The integer argument of a slot call - uniformBuffer<T>(slot) or
    storageBuffer<T>(slot). Evaluating arbitrary cursors crashes libclang, so
    only an integer literal or a reference to a constant is tried, and the
    reference is resolved to its declaration first. False when it is neither,
    which the caller turns into an error rather than a silent slot 0.
*/
bool evalSlotArg(CXCursor call, long long& slot) {
    struct Slot { long long* v; bool got; };
    Slot sl{ &slot, false };

    clang_visitChildren(call, [](CXCursor k, CXCursor, CXClientData d) {
        auto* sp = static_cast<Slot*>(d);
        if (sp->got) return CXChildVisit_Break;

        const CXCursorKind kk = clang_getCursorKind(k);
        CXCursor target = clang_getNullCursor();

        if (kk == CXCursor_IntegerLiteral) target = k;
        else if (kk == CXCursor_DeclRefExpr) {
            CXCursor ref = clang_getCursorReferenced(k);
            if (!clang_Cursor_isNull(ref) && clang_getCursorKind(ref) == CXCursor_VarDecl)
                target = ref;
        }

        if (clang_Cursor_isNull(target)) return CXChildVisit_Recurse;

        CXEvalResult ev = clang_Cursor_Evaluate(target);
        if (ev) {
            if (clang_EvalResult_getKind(ev) == CXEval_Int) {
                *sp->v = clang_EvalResult_getAsLongLong(ev);
                sp->got = true;
            }
            clang_EvalResult_dispose(ev);
        }
        return CXChildVisit_Recurse;
    }, &sl);

    return sl.got;
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


std::string stripFloatSuffix(const std::string& lit);

/*
    A default member initialiser is the field's last expression child. Only a
    plain expression is kept - a braced one would need its type spelled out,
    and a struct using one gets no defaults rather than wrong ones.
*/
std::string fieldInit(CXCursor field) {
    CXCursor expr = clang_getNullCursor();
    clang_visitChildren(field, [](CXCursor k, CXCursor, CXClientData d) {
        if (clang_isExpression(clang_getCursorKind(k))) *static_cast<CXCursor*>(d) = k;
        return CXChildVisit_Continue;
    }, &expr);
    if (clang_Cursor_isNull(expr)) return {};

    CXTranslationUnit tu = clang_Cursor_getTranslationUnit(field);
    CXToken* tokens = nullptr;
    unsigned count = 0;
    clang_tokenize(tu, clang_getCursorExtent(expr), &tokens, &count);

    std::string out;
    for (unsigned i = 0; i < count; ++i) {
        const std::string t = str(clang_getTokenSpelling(tu, tokens[i]));
        if (t == "{" || t == "}") { out.clear(); break; }
        out += clang_getTokenKind(tokens[i]) == CXToken_Literal ? stripFloatSuffix(t) : t;
    }
    clang_disposeTokens(tu, tokens, count);
    return out;
}


CXChildVisitResult visitFields(CXCursor c, CXCursor, CXClientData data) {
    if (clang_getCursorKind(c) == CXCursor_FieldDecl) {
        auto* s = static_cast<Struct*>(data);
        s->fields.push_back({
            str(clang_getTypeSpelling(clang_getCursorType(c))),
            str(clang_getCursorSpelling(c)),
            fieldInit(c)
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

        out->entryKey = startKey(c);
        out->entryFn  = c;
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

    if ((clang_getCursorKind(c) == CXCursor_FunctionDecl && clang_isCursorDefinition(c))
        || clang_getCursorKind(c) == CXCursor_VarDecl)
        if (isShaderSource(c)) out->declOrder.push_back({ startKey(c), out->nextOrder++ });

    /*
        Namespace scope only. Testing "parent is not a FunctionDecl" lets a
        lambda's variable through - its parent is the lambda, not the enclosing
        function - and it then gets emitted as a global and renamed, which also
        catches every unrelated local sharing its name.
    */
    if (clang_getCursorKind(c) == CXCursor_VarDecl
        && isShaderSource(c)
        && str(clang_getCursorSpelling(c)).rfind("valo_reg_", 0) != 0
    ) {
        const CXCursorKind parent = clang_getCursorSemanticParent(c).kind;
        if (parent == CXCursor_TranslationUnit || parent == CXCursor_Namespace) {
            out->globals.push_back(c);

            CXType vt = clang_getCursorType(c);
            if (vt.kind == CXType_ConstantArray)
                out->arrayLengths.push_back(
                    { str(clang_getCursorSpelling(c)), clang_getArraySize(vt) });
        }
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

    /*
        uniformBuffer<T>(slot) is how a shader reads a bound uniform buffer. The
        call's result type is const T&, and the slot is a constant, so both come
        straight off the cursor - and GLSL gets one block per type.
    */
    if (clang_getCursorKind(c) == CXCursor_CallExpr
        && str(clang_getCursorSpelling(c)) == "uniformBuffer"
    ) {
        /*
            Spelling rather than bareType: libclang leaves this cursor's type
            unexposed, and clang_getNonReferenceType dereferences such a type
            straight into a crash. getTypeSpelling is safe on anything, so the
            const and the reference come off by hand.
        */
        std::string ty = str(clang_getTypeSpelling(clang_getCursorType(c)));
        if (ty.rfind("const ", 0) == 0) ty = ty.substr(6);
        while (!ty.empty() && (ty.back() == '&' || ty.back() == ' ')) ty.pop_back();

        const size_t cc = ty.rfind("::");
        if (cc != std::string::npos) ty = ty.substr(cc + 2);

        if (ty.empty()) return CXChildVisit_Recurse;

        long long slot = 0;
        if (!evalSlotArg(c, slot)) {
            out->errors.push_back(where(c) + ": uniformBuffer<" + ty + ">() needs a "
                "compile-time slot - an integer literal or a constexpr constant");
            return CXChildVisit_Recurse;
        }

        /*
            GLSL gets one block per type, so the same type read from two slots
            would quietly bind only the first.
        */
        bool seen = false;
        for (const auto& ub : out->uniformBlocks) {
            if (ub.first != ty) continue;
            seen = true;
            if (ub.second != slot)
                out->errors.push_back(where(c) + ": uniformBuffer<" + ty + "> is read from slot "
                    + std::to_string(ub.second) + " and slot " + std::to_string(slot)
                    + " - one shader can bind a uniform type to only one slot");
        }
        if (!seen) out->uniformBlocks.push_back({ ty, slot });
    }

    // VALO_GLSL expands to `inline const char* valo_glsl_<fn> = "...";`
    // no isShaderSource test: the declaration comes from a macro, so its
    // spelling location resolves into ShaderRegistry.hpp rather than the shader
    if (clang_getCursorKind(c) == CXCursor_VarDecl
        && str(clang_getCursorSpelling(c)).rfind("valo_glsl_", 0) == 0
    ) {
        const std::string fn = str(clang_getCursorSpelling(c)).substr(10);

        std::string lit;
        clang_visitChildren(c, [](CXCursor k, CXCursor, CXClientData d) {
            if (clang_getCursorKind(k) == CXCursor_StringLiteral)
                *static_cast<std::string*>(d) = str(clang_getCursorSpelling(k));
            return CXChildVisit_Recurse;
        }, &lit);

        // the spelling keeps the surrounding quotes and the escapes
        if (lit.size() >= 2 && lit.front() == '"' && lit.back() == '"')
            lit = lit.substr(1, lit.size() - 2);

        std::string unescaped;
        for (size_t k = 0; k < lit.size(); ++k) {
            if (lit[k] == '\\' && k + 1 < lit.size()) {
                const char e = lit[++k];
                unescaped += (e == 'n') ? '\n' : (e == 't') ? '\t' : e;
            } else unescaped += lit[k];
        }

        if (getenv("VALOC_DEBUG"))
            std::fprintf(stderr, "[valoc] VALO_GLSL %s -> %zu chars\n", fn.c_str(), unescaped.size());
        if (!unescaped.empty()) out->glslOverrides.push_back({ fn, unescaped });
    }

    if (clang_getCursorKind(c) == CXCursor_VarDecl && isShaderSource(c)
        && [&]{
               std::string t = bareType(clang_getCursorType(c));
               return t == "Sampler" || (t.size() > 8 && t.compare(t.size() - 9, 9, "::Sampler") == 0);
           }()
    ) {
        out->samplerLocals.push_back(str(clang_getCursorSpelling(c)));
    }

    if (clang_getCursorKind(c) == CXCursor_VarDecl && isShaderSource(c)) {
        CXCursor lambda = clang_getNullCursor();
        clang_visitChildren(c, [](CXCursor k, CXCursor, CXClientData d) {
            if (clang_getCursorKind(k) == CXCursor_LambdaExpr) {
                *static_cast<CXCursor*>(d) = k;
                return CXChildVisit_Break;
            }
            return CXChildVisit_Recurse;
        }, &lambda);

        if (!clang_Cursor_isNull(lambda)) {
            // walk out to the enclosing function so the hoist lands above it
            CXCursor host = clang_getCursorSemanticParent(c);
            while (!clang_Cursor_isNull(host)
                && clang_getCursorKind(host) != CXCursor_FunctionDecl
                && clang_getCursorKind(host) != CXCursor_TranslationUnit)
                host = clang_getCursorSemanticParent(host);

            const std::string hostKey =
                clang_Cursor_isNull(host) || clang_getCursorKind(host) != CXCursor_FunctionDecl
                    ? std::string() : startKey(host);
            out->lambdas.push_back({ str(clang_getCursorSpelling(c)), lambda, hostKey });
        }
    }

    if (clang_getCursorKind(c) == CXCursor_StructDecl && clang_isCursorDefinition(c)) {
        Struct s{ str(clang_getCursorSpelling(c)), {} };
        clang_visitChildren(c, visitFields, &s);

        if (!s.name.empty()) {
            if (isShaderFile(c)) out->shaderStructs.push_back(s.name);
            out->structs.push_back(std::move(s));
        }
    }

    return CXChildVisit_Recurse;
}


/*
    A helper or constant from a plain project header is kept only when the
    shader reaches it: from the entry point, from anything in a shader file
    (all of which is emitted), or from another kept declaration. Unlike a
    shader file, a plain header may be mostly C++ that GLSL could never take.
*/
void pruneUnreached(Collect& c, CXCursor entryFn) {
    struct Walk {
        Collect* c;
        std::vector<std::string> reached;
        std::vector<CXCursor>    queue;
    } w{ &c, {}, {} };

    auto candidate = [&](CXCursor d) {
        for (CXCursor h : c.helpers) if (clang_equalCursors(h, d)) return true;
        for (CXCursor g : c.globals) if (clang_equalCursors(g, d)) return true;
        return false;
    };

    auto reach = [&](CXCursor d) {
        const std::string key = startKey(d);
        for (const auto& r : w.reached) if (r == key) return;
        w.reached.push_back(key);
        w.queue.push_back(d);
    };

    if (!clang_Cursor_isNull(entryFn)) w.queue.push_back(entryFn);
    for (CXCursor h : c.helpers) if (isShaderFile(h)) reach(h);
    for (CXCursor g : c.globals) if (isShaderFile(g)) reach(g);

    struct Ctx { Walk* w; std::vector<CXCursor>* found; };
    while (!w.queue.empty()) {
        CXCursor next = w.queue.back();
        w.queue.pop_back();

        std::vector<CXCursor> found;
        Ctx ctx{ &w, &found };
        clang_visitChildren(next, [](CXCursor k, CXCursor, CXClientData d) {
            const CXCursorKind kk = clang_getCursorKind(k);
            if (kk == CXCursor_TypeRef || kk == CXCursor_DeclRefExpr
                || kk == CXCursor_MemberRefExpr || kk == CXCursor_OverloadedDeclRef) {
                CXCursor ref = clang_getCursorReferenced(k);
                if (!clang_Cursor_isNull(ref)) {
                    CXCursor def = clang_getCursorDefinition(ref);
                    static_cast<Ctx*>(d)->found->push_back(clang_Cursor_isNull(def) ? ref : def);
                }
            }
            return CXChildVisit_Recurse;
        }, &ctx);

        for (CXCursor f : found) {
            if (candidate(f)) { reach(f); continue; }

            // a struct from a plain header that kept code uses: shader files'
            // structs are all emitted already, library ones by their own rule
            if (clang_getCursorKind(f) == CXCursor_StructDecl && isShaderSource(f) && !isShaderFile(f)) {
                const std::string name = str(clang_getCursorSpelling(f));
                bool have = name.empty();
                for (const auto& n : c.shaderStructs) if (n == name) have = true;
                if (!have) c.shaderStructs.push_back(name);
            }
        }
    }

    auto keep = [&](CXCursor d) {
        if (isShaderFile(d)) return true;
        const std::string key = startKey(d);
        for (const auto& r : w.reached) if (r == key) return true;
        return false;
    };

    std::vector<CXCursor> helpers, globals;
    for (CXCursor h : c.helpers) if (keep(h)) helpers.push_back(h);
    for (CXCursor g : c.globals) if (keep(g)) globals.push_back(g);
    c.helpers = std::move(helpers);
    c.globals = std::move(globals);
}


// the directory <render/ShaderRegistry.hpp> was found under, with a trailing /
std::string findLibraryRoot(CXTranslationUnit tu) {
    std::string root;
    clang_getInclusions(tu, [](CXFile f, CXSourceLocation*, unsigned, CXClientData d) {
        auto* out = static_cast<std::string*>(d);
        if (!out->empty()) return;

        std::string p = str(clang_getFileName(f));
        for (char& ch : p) if (ch == '\\') ch = '/';

        const std::string tail = "render/ShaderRegistry.hpp";
        if (p.size() >= tail.size() && p.compare(p.size() - tail.size(), tail.size(), tail) == 0)
            *out = p.substr(0, p.size() - tail.size());
    }, &root);
    return root;
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
    std::string t = colons == std::string::npos ? cxx : cxx.substr(colons + 2);

    /*
        Vec3f and friends are aliases for Vec3<float>, and clang spells whichever
        the declaration used - so a function returning Vec3f can come back as
        either. Fold the template spelling onto the alias before matching.
    */
    static const struct { const char* tmpl; const char* alias; } TEMPLATES[] = {
        { "Vec2<float>", "Vec2f" }, { "Vec3<float>", "Vec3f" }, { "Vec4<float>", "Vec4f" },
        { "Vec2<int>",   "Vec2i" }, { "Vec3<int>",   "Vec3i" }, { "Vec4<int>",   "Vec4i" },
        { "Vec2<unsigned int>", "Vec2u" }, { "Vec3<unsigned int>", "Vec3u" },
        { "Vec4<unsigned int>", "Vec4u" },
        { "Mat2<float>", "Mat2f" }, { "Mat3<float>", "Mat3f" }, { "Mat4<float>", "Mat4f" }
    };
    for (const auto& k : TEMPLATES) if (t == k.tmpl) { t = k.alias; break; }

    if (t == "float")    return "float";
    if (t == "double")   return "double";
    if (t == "bool")     return "bool";
    if (t == "int"    || t == "int32_t")  return "int";
    if (t == "unsigned int" || t == "uint32_t" || t == "TextureID"
     || t == "RenderTargetID" || t == "UniformBufferID" || t == "StorageBufferID"
     || t == "VertexBufferID" || t == "IndexBufferID" || t == "ProgramID")  return "uint";

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
    static const char* BUILTINS[] = {
        "float", "double", "bool", "int", "uint",
        "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4",
        "uvec2", "uvec3", "uvec4", "mat2", "mat3", "mat4"
    };
    for (const char* b : BUILTINS) if (glsl == b) return true;
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


/*
    GLSL function names a shader may legitimately define in C++. Redefining one
    is not an overload - glslc rejects it on precision qualifiers - so these get
    a valo_ prefix and every call site follows.
*/
bool isGlslBuiltinFunction(const std::string& n) {
    static const char* BUILTINS[] = {
        "abs", "sign", "floor", "ceil", "trunc", "round", "roundEven", "fract",
        "mod", "modf", "min", "max", "clamp", "mix", "step", "smoothstep",
        "sqrt", "inversesqrt", "pow", "exp", "log", "exp2", "log2",
        "sin", "cos", "tan", "asin", "acos", "atan", "sinh", "cosh", "tanh",
        "length", "distance", "dot", "cross", "normalize", "reflect", "refract",
        "faceforward", "transpose", "inverse", "determinant", "outerProduct",
        "texture", "textureLod", "textureSize", "texelFetch",
        "dFdx", "dFdy", "fwidth", "isnan", "isinf",
        "any", "all", "not", "equal", "notEqual",
        "lessThan", "lessThanEqual", "greaterThan", "greaterThanEqual"
    };
    for (const char* b : BUILTINS) if (n == b) return true;
    return false;
}


// vecN(...) and friends are constructors, not calls - a global const may use
// them and still be a constant expression
bool isGlslTypeConstructor(const std::string& n) {
    static const char* TYPES[] = {
        "float", "int", "uint", "bool",
        "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4",
        "uvec2", "uvec3", "uvec4", "bvec2", "bvec3", "bvec4",
        "mat2", "mat3", "mat4", "mat2x2", "mat2x3", "mat2x4",
        "mat3x2", "mat3x3", "mat3x4", "mat4x2", "mat4x3", "mat4x4"
    };
    for (const char* t : TYPES) if (n == t) return true;
    return false;
}


/*
    GLSL requires a global const initialiser to be a constant expression, so
    `const vec3 k = someHelper(...)` has to become a function instead. Anything
    that calls something other than a type constructor qualifies.
*/
bool initIsConstantExpression(const std::string& init) {
    for (size_t i = 0; i < init.size(); ++i) {
        if (init[i] != '(') continue;

        size_t e = i;
        while (e > 0 && (init[e - 1] == ' ' || init[e - 1] == '\t')) --e;
        size_t b = e;
        while (b > 0 && (std::isalnum((unsigned char)init[b - 1]) || init[b - 1] == '_')) --b;

        if (b == e) continue;                       // "(" with no callee: grouping
        const std::string name = init.substr(b, e - b);
        if (!isGlslTypeConstructor(name)) return false;
    }
    return true;
}


// GLSL keywords that are ordinary identifiers in C++, so a local named `out`
// or `in` compiles here and not there
bool isReservedInGlsl(const std::string& t) {
    static const char* RESERVED[] = {
        "in", "out", "inout", "uniform", "varying", "attribute", "buffer",
        "shared", "layout", "sampler", "image", "flat", "smooth", "noperspective",
        "centroid", "patch", "sample", "subroutine", "precision", "invariant",
        "lowp", "mediump", "highp", "common", "partition", "active",
        "half", "fixed", "hvec2", "hvec3", "hvec4", "fvec2", "fvec3", "fvec4",
        "dvec2", "dvec3", "dvec4", "cast", "namespace", "using", "this",
        "filter", "input", "output"
    };
    for (const char* r : RESERVED) if (t == r) return true;
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
        static const char* WRAP[] = { "REPEAT", "CLAMP", "MIRROR" };
        return std::string("VALO_") + WRAP[u] + (f ? "_LINEAR" : "_NEAREST");
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



/*
    C++ converts an integer to bool implicitly and GLSL does not, so a uint flag
    read as a condition - "if (!u.detail)", "u.useNormalMap && x" - is a type
    error once translated. Spelling the comparison out keeps the translation
    one to one:

        !x        ->  x == 0u
        x && y    ->  x != 0u && y
        if (x)    ->  if (x != 0u)

    Only member chains rooted at the uniform parameter are resolved, which is
    where flags live; a local of integer type used as a condition is not caught.
*/
bool isIntegerMemberChain(
    const Collect& c,
    unsigned start,
    unsigned count,
    CXToken* tokens,
    const std::string& uniformParam,
    const std::string& uniformType,
    unsigned& endsAt
) {
    if (str(clang_getTokenSpelling(c.tu, tokens[start])) != uniformParam) return false;

    const Struct* current = findStruct(c, uniformType);
    if (!current) return false;

    unsigned i = start;
    std::string fieldType;

    while (i + 2 < count
        && str(clang_getTokenSpelling(c.tu, tokens[i + 1])) == "."
    ) {
        const std::string field = str(clang_getTokenSpelling(c.tu, tokens[i + 2]));

        const Field* found = nullptr;
        for (const auto& f : current->fields) if (f.name == field) found = &f;
        if (!found) return false;

        fieldType = glslType(found->type);
        i += 2;

        if (fieldType == "uint" || fieldType == "int") { endsAt = i; return true; }

        current = findStruct(c, fieldType);
        if (!current) return false;
    }

    return false;
}


/*
    A storage buffer is declared inside the body in C++ and at file scope in
    GLSL:

        const StorageView<float> numbers = storageBuffer<float>(0);
            ->  layout(scalar, set = 2, binding = 0)
                    readonly buffer Storage0 { float data[]; } s0;

    so the declaration is lifted out and every use of the local name is
    rewritten onto the block. .size() becomes data.length() and [i] stays a
    subscript, which is the whole of the mapping.
*/
struct StorageDecl {
    std::string name;       // the local the shader uses
    std::string elemType;   // GLSL element type
    unsigned    slot = 0;
    std::string declKey;    // where the declaring statement starts, so it can be dropped
    unsigned    stride = 0; // element size under scalar layout, for .size()
};


/*
    Byte size of a GLSL type under scalar layout - every member aligned to its
    scalar component, so for the float/int types a shader uses this is the C++
    sizeof. 0 for anything it cannot size.
*/
unsigned scalarSize(const Collect& c, const std::string& glsl, int depth = 0) {
    if (glsl == "float" || glsl == "int" || glsl == "uint") return 4;
    if (glsl == "vec2"  || glsl == "ivec2" || glsl == "uvec2") return 8;
    if (glsl == "vec3"  || glsl == "ivec3" || glsl == "uvec3") return 12;
    if (glsl == "vec4"  || glsl == "ivec4" || glsl == "uvec4") return 16;
    if (glsl == "mat2") return 16;
    if (glsl == "mat3") return 36;
    if (glsl == "mat4") return 64;
    if (depth > 8) return 0;

    const Struct* st = findStruct(c, glsl);
    if (!st) return 0;

    unsigned total = 0;
    for (const Field& f : st->fields) {
        std::string ft = f.type;
        unsigned count = 1;

        const size_t bracket = ft.find('[');
        if (bracket != std::string::npos) {
            count = unsigned(std::atoi(ft.c_str() + bracket + 1));
            ft = ft.substr(0, bracket);
        }
        while (!ft.empty() && ft.back() == ' ') ft.pop_back();

        const unsigned each = scalarSize(c, glslType(ft), depth + 1);
        if (!each || !count) return 0;
        total += each * count;
    }
    return total;
}


/*
    Found from the AST rather than by matching tokens, so the spelling of the
    declaration does not matter - StorageView<T>, const StorageView<T>, auto and
    const auto all work - and the slot is evaluated exactly as uniformBuffer's
    is, so a named constant resolves instead of falling through to 0.
*/
struct StorageScan {
    const Collect*              c;
    std::vector<StorageDecl>*   out;
    std::vector<std::string>*   errors;
    CXCursor                    stmt;       // the DeclStmt being looked inside
    CXCursor                    var;        // its VarDecl
};


CXChildVisitResult findStorageCall(CXCursor k, CXCursor, CXClientData d) {
    auto* s = static_cast<StorageScan*>(d);

    if (clang_getCursorKind(k) != CXCursor_CallExpr
        || str(clang_getCursorSpelling(k)) != "storageBuffer"
    ) return CXChildVisit_Recurse;

    // the element type is the template argument: storageBuffer < T > ( slot )
    CXToken* tokens = nullptr;
    unsigned count = 0;
    clang_tokenize(s->c->tu, clang_getCursorExtent(k), &tokens, &count);

    std::string elem;
    int depth = 0;
    for (unsigned i = 0; i < count; ++i) {
        const std::string t = str(clang_getTokenSpelling(s->c->tu, tokens[i]));
        if (t == "<") { if (depth++ == 0) continue; }
        else if (t == ">") { if (--depth == 0) break; }
        if (depth > 0) elem += t;
    }
    clang_disposeTokens(s->c->tu, tokens, count);

    if (elem.rfind("const", 0) == 0) elem = elem.substr(5);

    long long slot = 0;
    if (!evalSlotArg(k, slot)) {
        if (s->errors)
            s->errors->push_back(where(k) + ": storageBuffer<" + elem + ">() needs a "
                "compile-time slot - an integer literal or a constexpr constant");
        return CXChildVisit_Break;
    }

    const std::string glsl = glslType(elem);
    const unsigned stride = scalarSize(*s->c, glsl);

    if (!stride && s->errors)
        s->errors->push_back(where(k) + ": storageBuffer<" + elem + ">: cannot work out its "
            "size in GLSL - elements must be made of float, int, uint and their vectors, "
            "matrices and structs");

    s->out->push_back({
        str(clang_getCursorSpelling(s->var)),
        glsl,
        unsigned(slot),
        startKey(s->stmt),
        stride
    });
    return CXChildVisit_Break;
}


CXChildVisitResult findStorageStmt(CXCursor k, CXCursor, CXClientData d) {
    auto* s = static_cast<StorageScan*>(d);

    if (clang_getCursorKind(k) != CXCursor_DeclStmt) return CXChildVisit_Recurse;

    s->stmt = k;
    clang_visitChildren(k, [](CXCursor v, CXCursor, CXClientData dd) {
        auto* ss = static_cast<StorageScan*>(dd);
        if (clang_getCursorKind(v) == CXCursor_VarDecl) {
            ss->var = v;
            clang_visitChildren(v, findStorageCall, ss);
        }
        return CXChildVisit_Continue;
    }, s);

    return CXChildVisit_Continue;
}


std::vector<StorageDecl> findStorageDecls(
    const Collect& c,
    CXCursor body,
    std::vector<std::string>* errors = nullptr
) {
    std::vector<StorageDecl> out;
    if (clang_Cursor_isNull(body)) return out;

    StorageScan scan{ &c, &out, errors, clang_getNullCursor(), clang_getNullCursor() };
    clang_visitChildren(body, findStorageStmt, &scan);
    return out;
}


std::vector<StorageDecl> findAllStorageDecls(
    const Collect& c,
    std::vector<std::string>* errors = nullptr
) {
    std::vector<StorageDecl> out = findStorageDecls(c, c.body, errors);

    for (CXCursor h : c.helpers) {
        CXCursor body = clang_getNullCursor();
        clang_visitChildren(h, [](CXCursor k, CXCursor, CXClientData d) {
            if (clang_getCursorKind(k) == CXCursor_CompoundStmt)
                *static_cast<CXCursor*>(d) = k;
            return CXChildVisit_Continue;
        }, &body);

        for (const auto& d : findStorageDecls(c, body, errors)) out.push_back(d);
    }

    return out;
}


std::string emitStorageBlocks(const std::vector<StorageDecl>& decls, const unsigned base) {
    std::string out;
    std::vector<unsigned> seen;

    for (const auto& d : decls) {
        bool already = false;
        for (unsigned v : seen) if (v == d.slot) already = true;
        if (already) continue;
        seen.push_back(d.slot);

        const std::string n = std::to_string(d.slot);
        // the same 16-byte header VulkanRenderer writes ahead of the data
        out += "layout(scalar, set = 0, binding = " + std::to_string(base + d.slot)
             + ") readonly buffer Storage" + n
             + " { uint byteSize; uint pad0; uint pad1; uint pad2; "
             + d.elemType + " data[]; } s" + n + ";\n";
    }

    if (!decls.empty()) out += "\n";
    return out;
}


/*
    A helper that takes the uniform struct has no GLSL equivalent: there the
    block is a global, not something you can pass. The parameter is dropped from
    the signature and the matching argument from every call, which leaves the
    body reading the block directly - it already spells it with the same name.
*/
struct DroppedArg {
    std::string function;
    int         index = -1;
};

/*
    The constructor call that gives a struct local its C++ defaults, or empty
    when the struct has none or one can't be written. A field without an
    initialiser is value-initialised, so it is zero, as in C++.
*/
std::string defaultConstruct(const Collect& c, const std::string& type, const bool nameFollows) {
    if (!nameFollows) return {};

    const Struct* s = findStruct(c, type);
    if (!s) return {};

    bool any = false;
    for (const Field& f : s->fields) if (!f.init.empty()) any = true;
    if (!any) return {};

    std::string args;
    for (const Field& f : s->fields) {
        std::string v = f.init;
        if (v.empty()) {
            const std::string g = glslType(f.type);
            if      (g == "float") v = "0.0";
            else if (g == "uint")  v = "0u";
            else if (g == "int")   v = "0";
            else if (g == "bool")  v = "false";
            else if (g.rfind("vec", 0) == 0 || g.rfind("ivec", 0) == 0
                  || g.rfind("uvec", 0) == 0 || g.rfind("mat", 0) == 0) v = g + "(0)";
            else return {};
        }
        args += (args.empty() ? "" : ", ") + v;
    }
    return glslType(s->name) + "(" + args + ")";
}


std::string translateRange(
    const Collect& c,
    CXCursor cursor,
    const std::vector<Rename>& renames,
    bool dropOuterBraces,
    const std::vector<StorageDecl>& storage = {},
    const std::vector<DroppedArg>& dropped = {}
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

    // the uniform parameter keeps its own name, so a member chain off it is
    // recognisable by spelling alone
    const std::string uniformParam = c.paramNames.size() > 1 ? c.paramNames[1] : std::string();
    const std::string uniformType  = c.paramTypes.size() > 1 ? c.paramTypes[1] : std::string();

    unsigned intChainEndsAt = count;    // token index that completes an integer chain
    bool     lastWasInt     = false;    // the expression just emitted was one

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

    // a range-for's element declaration, emitted just inside the loop body
    std::string pendingLoopElem;

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
        /*
            `auto f = [](...) { ... };` - hoisted to a top-level function named
            after the variable, so the declaration itself has no GLSL form. Call
            sites already spell that name and need no rewriting.
        */
        /*
            A storage declaration moved to file scope, so the statement that
            produced it is dropped. Matched by where the statement starts rather
            than by its tokens, so any spelling of it - const, auto, the full
            StorageView<T> - goes.
        */
        if (!storage.empty()) {
            const std::string key = tokenKey(c.tu, tokens[i]);
            bool isStorageDecl = false;
            for (const auto& d : storage) if (d.declKey == key) isStorageDecl = true;

            if (isStorageDecl) {
                while (i < count && str(clang_getTokenSpelling(c.tu, tokens[i])) != ";") ++i;
                continue;
            }
        }

        if (kind == CXToken_Identifier && !c.enumValues.empty()) {
            const std::string key = tokenKey(c.tu, tokens[i]);
            std::string value;
            for (const auto& [k, v] : c.enumValues) if (k == key) value = v;

            if (!value.empty()) {
                put(value, false);
                prevSpelling = value;
                continue;
            }
        }

        /*
            Any other `auto name = ...` gets its deduced type spelled out, which
            the collection pass worked out from the AST.
        */
        if (kind == CXToken_Keyword && t == "auto" && i + 3 < count) {
            unsigned nameAt = i + 1;
            if (str(clang_getTokenSpelling(c.tu, tokens[nameAt])) == "&") ++nameAt;

            const bool declares = nameAt + 2 < count
                && str(clang_getTokenSpelling(c.tu, tokens[nameAt + 1])) == "="
                && str(clang_getTokenSpelling(c.tu, tokens[nameAt + 2])) != "[";

            if (declares) {
                const std::string key = tokenKey(c.tu, tokens[nameAt]);
                std::string deduced;
                for (const auto& [k, ty] : c.autoTypes) if (k == key) deduced = ty;

                if (!deduced.empty()) {
                    put(deduced);
                    prevSpelling = deduced;
                    i = nameAt - 1;     // skips a trailing &, the name comes next
                    continue;
                }
            }
        }

        if (kind == CXToken_Keyword && t == "auto"
            && i + 3 < count
            && str(clang_getTokenSpelling(c.tu, tokens[i + 2])) == "="
            && str(clang_getTokenSpelling(c.tu, tokens[i + 3])) == "["
        ) {
            {
                std::string trimmed = out;
                while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
                if (trimmed.size() >= 5 && trimmed.compare(trimmed.size() - 5, 5, "const") == 0)
                    out = trimmed.substr(0, trimmed.size() - 5);
            }

            unsigned j = i;
            int d = 0;
            for (; j < count; ++j) {
                const std::string s2 = str(clang_getTokenSpelling(c.tu, tokens[j]));
                if (s2 == "{" || s2 == "(" || s2 == "[") ++d;
                else if (s2 == "}" || s2 == ")" || s2 == "]") --d;
                else if (s2 == ";" && d == 0) break;
            }
            i = j;
            continue;
        }

        /*
            GLSL has no references, and a local bound to one is only ever read,
            so `const T& x = y;` becomes a copy. Only dropped when the previous
            token actually names a type - otherwise this is a bitwise and.
        */
        if (t == "&" && i + 1 < count && !prevSpelling.empty()) {
            const bool prevIsType =
                glslType(prevSpelling) != prevSpelling || findStruct(c, prevSpelling);

            const CXTokenKind nextKind = clang_getTokenKind(tokens[i + 1]);
            if (prevIsType && nextKind == CXToken_Identifier) continue;
        }

        /*
            uniformBuffer<T>(slot) names the block declared above; the whole
            call, template argument and slot included, collapses to it.
        */
        if (t == "uniformBuffer" && i + 1 < count
            && str(clang_getTokenSpelling(c.tu, tokens[i + 1])) == "<"
        ) {
            unsigned j = i + 2;
            std::string ty;
            while (j < count && str(clang_getTokenSpelling(c.tu, tokens[j])) != ">")
                ty += str(clang_getTokenSpelling(c.tu, tokens[j++]));

            // skip past '>' and the parenthesised slot
            if (j < count) ++j;
            if (j < count && str(clang_getTokenSpelling(c.tu, tokens[j])) == "(") {
                int d2 = 0;
                for (; j < count; ++j) {
                    const std::string s2 = str(clang_getTokenSpelling(c.tu, tokens[j]));
                    if (s2 == "(") ++d2;
                    else if (s2 == ")") { if (--d2 == 0) break; }
                }
            }

            const size_t cc = ty.rfind("::");
            if (cc != std::string::npos) ty = ty.substr(cc + 2);

            tight("ub_" + ty, false);
            i = j;
            prevSpelling = ")";
            continue;
        }

        /*
            GLSL has no range-for, so `for (const T& e : arr)` becomes an indexed
            loop and the element is declared as the body's first statement. The
            length comes from the array's own declaration.
        */
        if (kind == CXToken_Keyword && t == "for"
            && i + 1 < count && str(clang_getTokenSpelling(c.tu, tokens[i + 1])) == "("
        ) {
            unsigned j = i + 1, d = 0, colon = 0;
            for (; j < count; ++j) {
                const std::string s2 = str(clang_getTokenSpelling(c.tu, tokens[j]));
                if (s2 == "(") ++d;
                else if (s2 == ")") { if (--d == 0) break; }
                else if (s2 == ":" && d == 1) colon = j;
            }

            if (colon) {
                // element name is the token before ':', array the one after
                const std::string elem = str(clang_getTokenSpelling(c.tu, tokens[colon - 1]));
                const std::string arr  = str(clang_getTokenSpelling(c.tu, tokens[colon + 1]));

                // the declared type, skipping const/& noise between '(' and the name
                std::string elemType;
                for (unsigned k = i + 2; k + 1 < colon; ++k) {
                    const std::string s3 = str(clang_getTokenSpelling(c.tu, tokens[k]));
                    if (s3 == "const" || s3 == "&" || s3 == "auto") continue;
                    elemType = glslType(s3);
                }

                long long len = 0;
                for (const auto& al : c.arrayLengths) if (al.first == arr) len = al.second;

                if (!elemType.empty() && len > 0) {
                    const std::string idx = elem + "_i";
                    put("for (int " + idx + " = 0; " + idx + " < "
                        + std::to_string(len) + "; ++" + idx + ")", true);

                    pendingLoopElem = elemType + " " + elem + " = " + arr + "[" + idx + "];";
                    i = j;
                    prevSpelling = ")";
                    continue;
                }
            }
        }

        // the declaration a sampler local came from; the name is already a
        // constant everywhere it is used
        if (t == "Sampler" && i + 1 < count) {
            unsigned j = i;
            while (j < count && str(clang_getTokenSpelling(c.tu, tokens[j])) != ";") ++j;

            bool isDecl = false;
            for (const std::string& n : c.samplerLocals)
                if (i + 1 < count && str(clang_getTokenSpelling(c.tu, tokens[i + 1])) == n)
                    isDecl = true;

            if (isDecl) {
                while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
                // "const" was already emitted ahead of the type
                const std::string tail = "const";
                if (out.size() >= tail.size()
                    && out.compare(out.size() - tail.size(), tail.size(), tail) == 0)
                    out.erase(out.size() - tail.size());
                i = j;
                continue;
            }
        }

        if (kind == CXToken_Keyword && t == "using") {
            while (i < count && str(clang_getTokenSpelling(c.tu, tokens[i])) != ";") ++i;
            continue;
        }

        // `T x;` default-constructs in C++, but a GLSL local starts undefined
        if (t == ";" && parenDepth == 0 && i >= 2) {
            const std::string ctor = defaultConstruct(c,
                str(clang_getTokenSpelling(c.tu, tokens[i - 2])),
                clang_getTokenKind(tokens[i - 1]) == CXToken_Identifier);
            if (!ctor.empty()) put("= " + ctor, false);
        }

        if (kind == CXToken_Literal) {
            put(stripFloatSuffix(t));
            prevSpelling = t;
            continue;
        }

        // a call to a helper that lost a parameter loses the argument with it
        if (kind == CXToken_Identifier && !dropped.empty()
            && i + 1 < count && str(clang_getTokenSpelling(c.tu, tokens[i + 1])) == "("
        ) {
            const DroppedArg* d = nullptr;
            for (const auto& e : dropped) if (e.function == t) d = &e;

            if (d && d->index == 0) {
                // only the first argument is handled: that is where a uniform
                // parameter is written, and a later one would need the commas
                // counted rather than just the opening paren skipped
                unsigned j = i + 2;
                int parens = 0;

                while (j < count) {
                    const std::string a = str(clang_getTokenSpelling(c.tu, tokens[j]));
                    if (a == "(" || a == "[") ++parens;
                    if (a == ")" || a == "]") { if (parens == 0) break; --parens; }
                    if (a == "," && parens == 0) { ++j; break; }
                    ++j;
                }

                put(t, false);
                out += "(";
                i = j - 1;
                ++parenDepth;
                prevSpelling = "(";
                continue;
            }
        }


        // and every use of the local name becomes the block it was lifted into
        if (kind == CXToken_Identifier && !storage.empty() && prevSpelling != ".") {
            const StorageDecl* decl = nullptr;
            for (const auto& d : storage) if (d.name == t) decl = &d;

            if (decl) {
                const std::string block = "s" + std::to_string(decl->slot);

                // .size() reads the header rather than .length(), which
                // MoltenVK reports as 0 through a push descriptor
                if (i + 3 < count
                    && str(clang_getTokenSpelling(c.tu, tokens[i + 1])) == "."
                    && str(clang_getTokenSpelling(c.tu, tokens[i + 2])) == "size"
                ) {
                    put("(" + block + ".byteSize / " + std::to_string(decl->stride) + "u)");
                    i += 4;                     // . size ( )
                    prevSpelling = ")";
                    continue;
                }

                put(block + ".data", false);
                prevSpelling = "data";
                continue;
            }
        }

        /*
            "!flag" is the only place the negation has to move: the operand is
            emitted first and the comparison takes its place, so the ! is
            dropped rather than translated.
        */
        if (t == "!" && i + 1 < count && !uniformParam.empty()) {
            unsigned endsAt = 0;
            if (isIntegerMemberChain(c, i + 1, count, tokens, uniformParam, uniformType, endsAt)) {
                std::string chain;
                for (unsigned k = i + 1; k <= endsAt; ++k)
                    chain += str(clang_getTokenSpelling(c.tu, tokens[k]));

                put(chain + " == 0u");
                i = endsAt;
                lastWasInt = false;     // it is a bool now
                prevSpelling = ")";
                continue;
            }
        }

        const bool wasInt = lastWasInt;
        lastWasInt = false;

        // "flag && x" needs the operand made boolean before the operator
        if ((t == "&&" || t == "||") && wasInt) {
            while (!out.empty() && out.back() == ' ') out.pop_back();
            out += " != 0u ";
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

            /*
                texture() picks its LOD the way the software backend does:
                analytically when the uv argument is a varying (in.uv), and
                mip 0 for anything computed. Implicit-LOD hardware sampling of
                a computed uv takes derivatives that jump at triangle edges.
            */
            if (t == "texture" && i + 1 < count
                && str(clang_getTokenSpelling(c.tu, tokens[i + 1])) == "("
                && !c.paramNames.empty()
            ) {
                // the second argument, as written
                std::vector<std::string> arg;
                int depth = 0, index = 0;

                for (unsigned j = i + 2; j < count; ++j) {
                    const std::string a = str(clang_getTokenSpelling(c.tu, tokens[j]));
                    if (a == "(" || a == "[" || a == "{") ++depth;
                    if (a == ")" || a == "]" || a == "}") { if (depth == 0) break; --depth; }
                    if (a == "," && depth == 0) { if (++index > 1) break; continue; }
                    if (index == 1) arg.push_back(a);
                }

                const bool isVarying = arg.size() == 3
                                    && arg[0] == c.paramNames[0]
                                    && arg[1] == ".";

                put(isVarying ? "valoTexture" : "valoTextureLod0", false);
                prevSpelling = "valoTexture";
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

            // record where this chain ends so the flag can be set once the
            // last field has actually been emitted
            if (!uniformParam.empty() && t == uniformParam) {
                unsigned endsAt = 0;
                intChainEndsAt = isIntegerMemberChain(
                    c, i, count, tokens, uniformParam, uniformType, endsAt) ? endsAt : count;
            }

            put(t);
            prevSpelling = t;
            lastWasInt = (i == intChainEndsAt);
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

            // the range-for element, now that the body is open
            if (!pendingLoopElem.empty()) {
                put(pendingLoopElem, false);
                newline();
                pendingLoopElem.clear();
            }
            prevSpelling = t;
            continue;
        }

        if (t == "}") {
            const bool call = !braceIsCall.empty() && braceIsCall.back();
            if (!braceIsCall.empty()) braceIsCall.pop_back();

            if (call) {
                // a trailing comma is legal in a C++ braced list and not in a
                // GLSL constructor call
                while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
                if (!out.empty() && out.back() == ',') out.pop_back();
                tight(")", true);
            }
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
    Free functions and constants, emitted ahead of main() in the order they were
    written - which is also a valid declaration order, because C++ required it.
*/

/*
    GLSL spells an array constant differently from C++:

        const Light LIGHTS[4] = Light[4]( Light(...), Light(...), ... );

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
std::string emitHelpers(
    const Collect& c,
    const std::vector<Rename>& renames,
    const std::vector<StorageDecl>& storage,
    const std::string& uniformTypeName,
    std::vector<DroppedArg>& dropped
) {
    std::string out;
    std::vector<std::string> emitted;

    enum class Kind { Global, Function, Lambda };
    struct Item { unsigned off; Kind kind; CXCursor cursor; std::string name; };
    std::vector<Item> items;

    // twice the visit order, leaving the odd slot below each for its lambdas
    auto orderOf = [&](const std::string& key) -> unsigned {
        for (const auto& [k, n] : c.declOrder) if (k == key) return n * 2;
        return 0;
    };

    for (CXCursor g : c.globals) items.push_back({ orderOf(startKey(g)), Kind::Global,   g, "" });
    for (CXCursor h : c.helpers) items.push_back({ orderOf(startKey(h)), Kind::Function, h, "" });

    // one below the host, so a hoisted lambda sorts immediately above the
    // function whose body declared it - and after anything that body calls.
    // A lambda in a helper that was pruned goes with it.
    for (const auto& l : c.lambdas) {
        if (!l.hostKey.empty()) {
            bool hostEmitted = l.hostKey == c.entryKey;
            for (CXCursor h : c.helpers) if (startKey(h) == l.hostKey) hostEmitted = true;
            if (!hostEmitted) continue;
        }
        const unsigned host = l.hostKey.empty() ? 0 : orderOf(l.hostKey);
        items.push_back({ host ? host - 1 : 0, Kind::Lambda, l.expr, l.name });
    }

    std::stable_sort(items.begin(), items.end(),
        [](const Item& a, const Item& b) { return a.off < b.off; });

    for (const Item& item : items) {
        if (item.kind == Kind::Lambda) {
            CXCursor lam = item.cursor;

            /*
                A LambdaExpr exposes its parameters and body as direct children
                and nothing else - its type is the closure class, so neither
                clang_getCursorResultType nor getNumArguments says anything. The
                return type therefore comes from the return expression.
            */
            struct Parts {
                std::vector<CXCursor> params;
                CXCursor body = clang_getNullCursor();
            } parts;

            clang_visitChildren(lam, [](CXCursor k, CXCursor, CXClientData d) {
                auto* pp = static_cast<Parts*>(d);
                if (clang_getCursorKind(k) == CXCursor_ParmDecl) pp->params.push_back(k);
                else if (clang_getCursorKind(k) == CXCursor_CompoundStmt) pp->body = k;
                return CXChildVisit_Continue;
            }, &parts);

            if (clang_Cursor_isNull(parts.body)) continue;

            CXType retType = clang_getCursorType(parts.body);   // overwritten below
            bool haveRet = false;
            struct RetFind { CXType* t; bool* found; } rf{ &retType, &haveRet };

            clang_visitChildren(parts.body, [](CXCursor k, CXCursor, CXClientData d) {
                auto* r = static_cast<RetFind*>(d);
                if (*r->found) return CXChildVisit_Break;
                if (clang_getCursorKind(k) != CXCursor_ReturnStmt) return CXChildVisit_Recurse;

                clang_visitChildren(k, [](CXCursor e, CXCursor, CXClientData d2) {
                    auto* r2 = static_cast<RetFind*>(d2);
                    *r2->t = clang_getCursorType(e);
                    *r2->found = true;
                    return CXChildVisit_Break;
                }, d);
                return CXChildVisit_Break;
            }, &rf);

            const std::string ret = haveRet
                ? glslType(bareType(retType))
                : std::string("void");

            out += ret + " " + item.name + "(";
            for (size_t i = 0; i < parts.params.size(); ++i) {
                if (i) out += ", ";
                out += glslType(bareType(clang_getCursorType(parts.params[i]))) + " "
                     + str(clang_getCursorSpelling(parts.params[i]));
            }
            out += ") {\n";
            out += translateRange(c, parts.body, renames, true);
            out += "}\n\n";
            continue;
        }

        if (item.kind == Kind::Global) {
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

            /*
                GLSL will not take a global const initialised by a function call,
                so it becomes a function and every reference gains "()". The
                rename that does that is registered by the caller, which has
                already scanned for this.
            */
            if (!initIsConstantExpression(value))
                out += type + " " + name + "() {\n    return" + value + ";\n}\n\n";
            else
                out += "const " + type + " " + name + " =" + value + ";\n";
            continue;
        }

        CXCursor h = item.cursor;

        /*
            A helper named after a GLSL builtin is a C++-side polyfill for it -
            fract, sign, mix, dot, length and friends exist only because C++ has
            no such function. GLSL does, so the definition is dropped and every
            call binds to the builtin. Emitting it instead is a redefinition,
            not an overload, and glslc rejects it.
        */
        if (isGlslBuiltinFunction(str(clang_getCursorSpelling(h)))) continue;

        // VALO_GLSL override: emit the hand-written GLSL, skip the C++ body
        {
            const std::string hn = str(clang_getCursorSpelling(h));
            bool overridden = false;
            for (const auto& o : c.glslOverrides)
                if (o.first == hn) { out += o.second + "\n\n"; overridden = true; break; }
            if (overridden) continue;
        }

        const CXType fn = clang_getCursorType(h);
        const std::string ret = glslType(str(clang_getTypeSpelling(clang_getResultType(fn))));

        const std::string fnName = str(clang_getCursorSpelling(h));
        out += ret + " " + fnName + "(";

        const int n = clang_Cursor_getNumArguments(h);
        bool wroteOne = false;

        for (int i = 0; i < n; ++i) {
            CXCursor arg = clang_Cursor_getArgument(h, i);
            const std::string argType = bareType(clang_getCursorType(arg));

            // the uniform block is a global in GLSL, so a parameter of that
            // type is dropped and the body reads the block instead
            if (!uniformTypeName.empty() && argType == uniformTypeName) {
                dropped.push_back({ fnName, i });
                continue;
            }

            /*
                GLSL passes parameters by value unless told otherwise, so a
                C++ out-parameter (T&, not const T&) has to say inout or every
                write the function makes is silently discarded.
            */
            const CXType argCx = clang_getCursorType(arg);
            const bool writable =
                argCx.kind == CXType_LValueReference
             && !clang_isConstQualifiedType(clang_getPointeeType(argCx));

            if (wroteOne) out += ", ";
            out += std::string(writable ? "inout " : "")
                 + glslType(argType) + " " + str(clang_getCursorSpelling(arg));
            wroteOne = true;
        }

        out += ") {\n";

        CXCursor body = clang_getNullCursor();
        clang_visitChildren(h, [](CXCursor k, CXCursor, CXClientData d) {
            if (clang_getCursorKind(k) == CXCursor_CompoundStmt)
                *static_cast<CXCursor*>(d) = k;
            return CXChildVisit_Continue;
        }, &body);

        // scoped to this function: another one may use the same name for
        // something else entirely
        out += translateRange(c, body, renames, true, findStorageDecls(c, body), dropped);
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

        // whatever its fields use goes first - a struct borrowed from a library
        // header (ModelMaterial, say) is otherwise only emitted when the entry
        // point reaches it, and this one may not be the entry's at all
        emitStructs(out, c, st, done);

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

    const std::vector<StorageDecl> storage = findAllStorageDecls(c);
    out += emitStorageBlocks(storage, c.storageBindingBase);

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
    renames.push_back({ "fmod",           "mod" });
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

    /*
        GLSL will not accept a global const whose initialiser calls a function,
        so those become zero-arg functions (see emitHelpers) and every reference
        needs the call parens. Translating the initialiser here is the only way
        to know which ones qualify, so it happens twice - once to classify, once
        to emit.
    */
    for (CXCursor g : c.globals) {
        CXType vt = clang_getCursorType(g);
        if (vt.kind == CXType_ConstantArray) continue;

        std::string raw = str(clang_getTypeSpelling(vt));
        if (raw.rfind("const ", 0) == 0) raw = raw.substr(6);
        const std::string type = glslType(raw);

        // scalars are constant-folded by emitHelpers, so they never need this
        if (type == "int" || type == "uint" || type == "float") continue;

        const std::string init = translateRange(c, g, renames, false);
        const size_t eq = init.find('=');
        if (eq == std::string::npos) continue;

        if (!initIsConstantExpression(init.substr(eq + 1))) {
            const std::string name = str(clang_getCursorSpelling(g));
            renames.push_back({ name, name + "()" });
        }
    }

    /*
        One uniform block per type a shader reads with uniformBuffer<T>(slot).
        The instance is named after the type so the call can collapse to it, and
        the fields are inlined rather than nested so `.exposure` still resolves.
    */
    for (const auto& [ty, slot] : c.uniformBlocks) {
        const Struct* st = findStruct(c, ty);
        if (!st) continue;

        /*
            The struct nested inside the block rather than its fields inlined,
            so the whole thing is a value: `const T x = uniformBuffer<T>(n);`
            translates to a struct copy, and a block cannot be assigned. The
            define is what lets the call site stay a single name.
        */
        out += "layout(scalar, set = 0, binding = " + std::to_string(slot)
             + ") uniform " + ty + "_Block {\n"
             + "    " + ty + " value;\n"
             + "} " + ty + "_buf;\n\n"
             + "#define ub_" + ty + " " + ty + "_buf.value\n\n";
        (void)st;
    }

    // a default-constructed Sampler is Repeat/Repeat/Linear
    for (const std::string& n : c.samplerLocals)
        renames.push_back({ n, samplerConstant({}) });

    std::vector<DroppedArg> dropped;
    out += emitHelpers(c, renames, storage, c.paramTypes[1], dropped);

    out += "void main() {\n";
    out += translateRange(c, c.body, renames, true, findStorageDecls(c, c.body), dropped);
    out += "}\n";
    return out;
}

} // namespace



/*
    The deduced type of every `auto` declaration in a shader file, keyed by the
    name's location so translateRange can spell it out. A type with no GLSL
    spelling is refused here, where the message can name the line, instead of
    surfacing later as a glslc error about a declaration that went missing.
*/
void collectAutoTypes(CXCursor root, Collect& c) {
    clang_visitChildren(root, [](CXCursor k, CXCursor, CXClientData d) {
        auto* cc = static_cast<Collect*>(d);
        if (clang_getCursorKind(k) != CXCursor_VarDecl || !isShaderSource(k))
            return CXChildVisit_Recurse;

        // only declarations actually spelled with auto
        CXToken* tokens = nullptr;
        unsigned count = 0;
        clang_tokenize(cc->tu, clang_getCursorExtent(k), &tokens, &count);

        bool isAuto = false;
        for (unsigned i = 0; i < count && i < 3; ++i) {
            const std::string t = str(clang_getTokenSpelling(cc->tu, tokens[i]));
            if (t == "auto") isAuto = true;
            if (t == "=") break;
        }
        clang_disposeTokens(cc->tu, tokens, count);
        if (!isAuto) return CXChildVisit_Recurse;

        std::string ty = str(clang_getTypeSpelling(clang_getCanonicalType(clang_getCursorType(k))));

        // lambdas are hoisted, storage views lifted - neither needs a type
        if (ty.find("lambda") != std::string::npos || ty.find("StorageView") != std::string::npos)
            return CXChildVisit_Recurse;

        if (ty.rfind("const ", 0) == 0) ty = ty.substr(6);
        while (!ty.empty() && (ty.back() == '&' || ty.back() == ' ')) ty.pop_back();

        const std::string glsl = glslType(ty);

        if (glsl.empty() || glsl.find_first_of("<>:(") != std::string::npos) {
            cc->errors.push_back(where(k) + ": `auto " + str(clang_getCursorSpelling(k))
                + "` deduces " + ty + ", which has no GLSL spelling - write the type out");
            return CXChildVisit_Recurse;
        }

        cc->autoTypes.push_back({ locationKey(clang_getCursorLocation(k)), glsl });
        return CXChildVisit_Recurse;
    }, &c);
}


/*
    GLSL has no enums, so every use of an enum constant - AlphaMode::Mask, or a
    shader's own enum - becomes its integer value. Unsigned when the enum's
    underlying type is, so it compares against a uint field without a cast.
*/
void collectEnumValues(CXCursor root, Collect& c) {
    clang_visitChildren(root, [](CXCursor k, CXCursor, CXClientData d) {
        auto* cc = static_cast<Collect*>(d);
        if (clang_getCursorKind(k) != CXCursor_DeclRefExpr || !isShaderSource(k))
            return CXChildVisit_Recurse;

        CXCursor ref = clang_getCursorReferenced(k);
        if (clang_Cursor_isNull(ref) || clang_getCursorKind(ref) != CXCursor_EnumConstantDecl)
            return CXChildVisit_Recurse;

        // canonical: an enum based on uint32_t reports the typedef, not its kind
        const CXType under = clang_getCanonicalType(
            clang_getEnumDeclIntegerType(clang_getCursorSemanticParent(ref)));
        const bool isUnsigned =
            under.kind == CXType_UInt  || under.kind == CXType_UShort ||
            under.kind == CXType_UChar || under.kind == CXType_ULong  ||
            under.kind == CXType_ULongLong || under.kind == CXType_Char_U;

        const std::string value = isUnsigned
            ? std::to_string(clang_getEnumConstantDeclUnsignedValue(ref)) + "u"
            : std::to_string(clang_getEnumConstantDeclValue(ref));

        cc->enumValues.push_back({ locationKey(clang_getCursorLocation(k)), value });
        return CXChildVisit_Recurse;
    }, &c);
}


/*
    A C++ bool is 1 byte and a bool inside a GLSL block is 4, so scalar layout
    cannot line them up: the bool and every field after it are read from the
    wrong offset on Vulkan, with nothing reporting it. Refused in every struct
    the GPU reads as a block - the per-draw uniforms, uniform buffers and
    storage elements - including structs nested inside them.
*/
void refuseBools(Collect& c, const std::string& type, const std::string& role,
                 const std::string& path, int depth = 0) {
    if (depth > 8) return;

    const Struct* st = findStruct(c, type);
    if (!st) return;

    for (const Field& f : st->fields) {
        std::string ft = f.type;
        const size_t bracket = ft.find('[');
        if (bracket != std::string::npos) ft = ft.substr(0, bracket);
        while (!ft.empty() && ft.back() == ' ') ft.pop_back();

        const std::string glsl = glslType(ft);
        const std::string here = path + "." + f.name;

        if (glsl == "bool") {
            c.errors.push_back(role + " field " + here + " is bool - a C++ bool is 1 byte but "
                "a GLSL block bool is 4, so it and everything after it are read from the "
                "wrong offset on Vulkan. Use uint32_t (test it with != 0)");
        } else {
            refuseBools(c, glsl, role, here, depth + 1);
        }
    }
}


/*
    The software backend interpolates every varying as a float, so an integer
    field - a TextureID handed from vertex to fragment, say - arrives as the
    bits of a blended float. sizeof cannot tell uint32_t from float, so the C++
    side cannot refuse it; this can.
*/
void refuseNonFloatVaryings(Collect& c, const std::string& type, const std::string& path,
                            int depth = 0) {
    if (depth > 8) return;

    const Struct* st = findStruct(c, type);
    if (!st) return;

    static const char* FLOAT_TYPES[] = {
        "float", "vec2", "vec3", "vec4", "mat2", "mat3", "mat4"
    };

    for (const Field& f : st->fields) {
        std::string ft = f.type;
        const size_t bracket = ft.find('[');
        if (bracket != std::string::npos) ft = ft.substr(0, bracket);
        while (!ft.empty() && ft.back() == ' ') ft.pop_back();

        const std::string glsl = glslType(ft);
        const std::string here = path + "." + f.name;

        bool isFloat = false;
        for (const char* k : FLOAT_TYPES) if (glsl == k) isFloat = true;

        if (isFloat) continue;

        if (findStruct(c, glsl)) {
            refuseNonFloatVaryings(c, glsl, here, depth + 1);
            continue;
        }

        c.errors.push_back("varyings field " + here + " is " + f.type + " - varyings are "
            "interpolated as floats, so integers and bools arrive garbled. Pass it through "
            "Uniforms instead, or as a float");
    }
}


// a namespace-scope integer constant from anywhere in the translation unit
bool findIntConstant(CXCursor root, const std::string& name, long long& value) {
    struct Find { std::string name; long long* v; bool got; };
    Find f{ name, &value, false };

    clang_visitChildren(root, [](CXCursor k, CXCursor, CXClientData d) {
        auto* ff = static_cast<Find*>(d);
        if (ff->got) return CXChildVisit_Break;

        const CXCursorKind kind = clang_getCursorKind(k);
        if (kind == CXCursor_Namespace) return CXChildVisit_Recurse;
        if (kind != CXCursor_VarDecl || str(clang_getCursorSpelling(k)) != ff->name)
            return CXChildVisit_Continue;

        CXEvalResult ev = clang_Cursor_Evaluate(k);
        if (ev) {
            if (clang_EvalResult_getKind(ev) == CXEval_Int) {
                *ff->v = clang_EvalResult_getAsLongLong(ev);
                ff->got = true;
            }
            clang_EvalResult_dispose(ev);
        }
        return CXChildVisit_Continue;
    }, &f);

    return f.got;
}


/*
    Every project file the translation read, so the backend can tell when a
    generated shader is stale - including headers a shader only borrows a
    struct from, like GLTFVertex. System and ext/ headers never change under a
    running project, so they are left out.
*/
std::vector<std::string> sourceDeps(CXTranslationUnit tu, const std::string& output) {
    namespace fs = std::filesystem;

    struct Deps { CXTranslationUnit tu; std::vector<std::string> paths; std::string cwd; fs::path base; };
    Deps deps;
    deps.tu   = tu;
    deps.cwd  = fs::current_path().generic_string() + "/";
    deps.base = output.empty() ? fs::path() : fs::absolute(output).parent_path();

    clang_getInclusions(tu, [](CXFile f, CXSourceLocation*, unsigned, CXClientData d) {
        auto* dp = static_cast<Deps*>(d);

        // a header found through a system include path never changes under a
        // running project; anything else might, wherever it lives - a shader
        // in another project's directory included
        if (clang_Location_isInSystemHeader(clang_getLocationForOffset(dp->tu, f, 0))) return;

        std::string p = str(clang_getFileName(f));
        for (char& ch : p) if (ch == '\\') ch = '/';

        // vendored libraries come in through -I, not -isystem, so the path is
        // the only way to tell them apart
        if (p.rfind("ext/", 0) == 0 || p.find("/ext/") != std::string::npos) return;

        if (!dp->base.empty()) {
            p = fs::absolute(p).lexically_normal().lexically_relative(dp->base).generic_string();
        } else {
            if (p.rfind(dp->cwd, 0) == 0) p = p.substr(dp->cwd.size());
            p = fs::path(p).lexically_normal().generic_string();
        }

        for (const std::string& have : dp->paths) if (have == p) return;
        dp->paths.push_back(p);
    }, &deps);

    return deps.paths;
}

/*
    The same rules the Vulkan backend applies before translating at runtime:
    missing is stale, hand-written (no marker) never is, and generated is stale
    when valoc or anything it was translated from is newer than it.
*/
bool isStale(const std::string& glslPath, const std::string& self) {
    namespace fs = std::filesystem;

    if (!fs::exists(glslPath)) return true;

    std::ifstream in(glslPath);
    std::string line;
    bool generated = false;
    std::vector<std::string> deps;

    for (int n = 0; n < 256 && std::getline(in, line); ++n) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.rfind("// generated by valoc", 0) == 0)  generated = true;
        else if (line.rfind("// valoc-dep: ", 0) == 0)    deps.push_back(line.substr(14));
        else if (generated && !line.empty() && line.rfind("//", 0) != 0) break;
    }

    if (!generated)   return false;
    if (deps.empty()) return true;

    std::error_code ec;
    const auto built = fs::last_write_time(glslPath, ec);
    if (ec) return true;

    for (const std::string& exe : {self, self + ".exe"}) {
        const auto t = fs::last_write_time(exe, ec);
        if (!ec) { if (t > built) return true; break; }
    }

    const fs::path dir = fs::path(glslPath).parent_path();
    for (const std::string& d : deps) {
        const auto t = fs::last_write_time(dir / d, ec);
        if (ec || t > built) return true;
    }

    return false;
}


int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: valoc <shader.hpp> [-o out.glsl] [-I dir]... [--if-stale]\n");
        return 2;
    }

    const std::string input = argv[1];
    std::string output;
    std::vector<std::string> includes;
    bool ifStale = false;

    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-o") && i + 1 < argc)      output = argv[++i];
        else if (!std::strcmp(argv[i], "-I") && i + 1 < argc) includes.push_back(argv[++i]);
        else if (!std::strncmp(argv[i], "-I", 2))             includes.push_back(argv[i] + 2);
        else if (!std::strcmp(argv[i], "--if-stale"))         ifStale = true;
    }

    if (ifStale && !output.empty() && !isStale(output, argv[0])) return 0;

    const Stage stage = stageOf(input);
    if (stage == Stage::Unknown) {
        std::fprintf(stderr, "valoc: %s: name must contain .vert. or .frag.\n", input.c_str());
        return 2;
    }

    std::vector<std::string> argStrings{ "-std=c++20", "-xc++" };

#ifdef VALOC_CLANG_RESOURCE
    // Baked in at configure time: libclang does not find its own builtins.
    // -resource-dir rather than -I, so clang places them after libc++ in the
    // search order - ahead of it, <cstddef> picks up the wrong stddef.h.
    argStrings.push_back(std::string("-resource-dir=") + VALOC_CLANG_RESOURCE);
#endif

#ifdef VALOC_SYSTEM_INCLUDES
    /*
        The compiler's own system include path, baked in by CMake. libclang does
        not inherit it, and the shader headers reach <vector> and <cmath> through
        VALO's own headers, so without this nothing parses.
    */
    {
        const std::string all = VALOC_SYSTEM_INCLUDES;
        size_t start = 0;
        while (start < all.size()) {
            const size_t end = all.find('|', start);
            if (end == std::string::npos) break;
            if (end > start) argStrings.push_back("-isystem" + all.substr(start, end - start));
            start = end + 1;
        }
    }
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
            g_libraryRoot = findLibraryRoot(tu);

            Collect collected;
            collected.entry      = entry;
            collected.tu         = tu;
            collected.allEntries = entries;
            clang_visitChildren(root, visitAll, &collected);

            pruneUnreached(collected, collected.entryFn);
            collectAutoTypes(root, collected);
            collectEnumValues(root, collected);

            long long uniformSlots = 0;
            if (findIntConstant(root, "MAX_UNIFORM_SLOTS", uniformSlots))
                collected.storageBindingBase = unsigned(uniformSlots);
            else
                collected.errors.push_back("MAX_UNIFORM_SLOTS not found - a shader must "
                    "include <render/ShaderRegistry.hpp>");

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
                const std::string uniformType = collected.paramTypes[1];
                refuseBools(collected, uniformType, "uniforms", uniformType);

                const std::string varyingType =
                    collected.paramTypes[stage == Stage::Vertex ? 2 : 0];
                refuseNonFloatVaryings(collected, varyingType, varyingType);

                for (const auto& [ty, slot] : collected.uniformBlocks)
                    refuseBools(collected, ty, "uniformBuffer<" + ty + ">", ty);

                for (const StorageDecl& d : findAllStorageDecls(collected, &collected.errors))
                    refuseBools(collected, d.elemType, "storageBuffer<" + d.elemType + ">", d.elemType);

                // the same uniform struct can be reached from more than one place
                std::vector<std::string> unique;
                for (const std::string& e : collected.errors) {
                    bool dup = false;
                    for (const std::string& u : unique) if (u == e) dup = true;
                    if (!dup) unique.push_back(e);
                }

                for (const std::string& e : unique) std::fprintf(stderr, "valoc: %s\n", e.c_str());

                if (!unique.empty()) {
                    std::fprintf(stderr, "valoc: %s: %zu error(s), refusing to translate\n",
                        input.c_str(), unique.size());
                    rc = 1;
                }
            }

            if (rc == 0) {
                std::string glsl = emit(collected, stage);

                const std::string marker = "// generated by valoc - do not edit\n";
                const size_t at = glsl.find(marker);
                if (at != std::string::npos) {
                    std::string deps;
                    for (const std::string& d : sourceDeps(tu, output)) deps += "// valoc-dep: " + d + "\n";
                    glsl.insert(at + marker.size(), deps);
                }

                if (output.empty()) {
                    std::printf("\n%s", glsl.c_str());
                } else if (std::FILE* f = std::fopen(output.c_str(), "wb")) {
                    std::fwrite(glsl.data(), 1, glsl.size(), f);
                    std::fclose(f);
                    std::printf("  %-12s %s\n", "wrote", output.c_str());
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
