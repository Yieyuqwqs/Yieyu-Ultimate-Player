// [7] 自研 Lua 子集解释器实现（零依赖，不用 LuaJIT）。
//
// 结构：词法 → 递归下降语法树 → 树遍历求值。全程返回状态码（本库编 -fno-exceptions，不用 throw）。
// 沙箱约束：调用深度 ≤ 32、单次脚本指令数 ≤ 2000 万、只暴露 lua_sandbox 白名单里的宿主函数。
#include "minilua.h"

#include <cmath>
#include <ctime>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../common/log.h"

#if defined(__cplusplus)
extern "C" {
#endif
// 模块 5 的 Go 出口（搜歌/取直链/脚本 HTTP 都走它）
char* yieyu_fetch_ex(const char* url, const char* method, const char* body, const char* headers,
                     int* outLen, int* outStatus);
void  yieyu_fetch_free(char* p);
#if defined(__cplusplus)
}
#endif

namespace {

// ══════════════ 值 ══════════════
struct Table;
struct Func;

struct Value {
  enum T { NIL, BOOL, NUM, STR, TAB, FN } t = NIL;
  bool b = false;
  double n = 0;
  std::string s;
  std::shared_ptr<Table> tab;
  std::shared_ptr<Func> fn;
  int host = -1;   // 宿主函数 id
};

struct Table {
  std::map<std::string, Value> map;   // 规范化键 → 值
  std::vector<std::string> order;     // 插入顺序（pairs/ipairs 用）
};

struct Func {
  std::vector<std::string> params;
  std::vector<struct Node*> body;
  std::shared_ptr<struct Env> closure;
};

// 键规范化：数字 → "n:<整数>"（便于上数组），字符串 → "s:<原串>"
std::string keyOfStr(const std::string& s) { return "s:" + s; }
std::string keyOfNum(double d) {
  char buf[40];
  if (d == std::floor(d) && std::fabs(d) < 1e15) {
    std::snprintf(buf, sizeof(buf), "n:%.0f", d);
  } else {
    std::snprintf(buf, sizeof(buf), "n:%.17g", d);
  }
  return std::string(buf);
}

// ══════════════ 语法树 ══════════════
struct Node {
  enum K {
    N_NUM, N_STR, N_NIL, N_BOOL, N_NAME, N_TABLE, N_INDEX, N_CALL, N_METHOD, N_UN, N_BIN, N_FUNC,
    S_LOCAL, S_ASSIGN, S_CALLSTMT, S_IF, S_WHILE, S_FORNUM, S_FORIN, S_RETURN, S_BREAK, S_DO,
    S_LOCALFUNC, S_FUNCDECL
  } k;
  double num = 0;
  bool b = false;
  std::string str;                   // 名字 / 字符串 / 运算符
  std::vector<Node*> kids;           // 子节点
  std::vector<std::string> names;    // 变量名 / 参数名
  std::vector<std::vector<Node*>> blocks;  // if 分支体等
  int line = 0;
  Node(K kk) : k(kk) {}
};

// ══════════════ 词法 ══════════════
struct Tok {
  enum T { END, NAME, NUM, STR, OP, KW } t = END;
  std::string v;
  double num = 0;
  int line = 1;
};

struct Lexer {
  const char* p;
  size_t n;
  size_t i = 0;
  int line = 1;
  std::string err;

  bool isId0(char c) const { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
  bool isId(char c) const { return isId0(c) || (c >= '0' && c <= '9'); }

  static bool isKw(const std::string& w) {
    static const char* kws[] = {"and", "break", "do", "else", "elseif", "end", "false", "for",
                                "function", "if", "in", "local", "nil", "not", "or", "return",
                                "then", "true", "until", "while", "repeat"};
    for (const char* k : kws) if (w == k) return true;
    return false;
  }

  Tok next() {
    Tok t;
    while (i < n) {
      const char c = p[i];
      if (c == '\n') { ++line; ++i; continue; }
      if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
      if (c == '-' && i + 1 < n && p[i + 1] == '-') {
        i += 2;
        if (i + 1 < n && p[i] == '[' && p[i + 1] == '[') {
          i += 2;
          while (i + 1 < n && !(p[i] == ']' && p[i + 1] == ']')) ++i;
          i += 2;
        } else {
          while (i < n && p[i] != '\n') ++i;
        }
        continue;
      }
      break;
    }
    if (i >= n) { t.t = Tok::END; return t; }
    const char c = p[i];
    t.line = line;
    if (isId0(c)) {
      size_t j = i;
      while (j < n && isId(p[j])) ++j;
      t.v.assign(p + i, j - i);
      t.t = isKw(t.v) ? Tok::KW : Tok::NAME;
      i = j;
      return t;
    }
    if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < n && p[i + 1] >= '0' && p[i + 1] <= '9')) {
      size_t j = i;
      while (j < n) {
        const char d = p[j];
        if ((d >= '0' && d <= '9') || d == '.' || d == 'x' || d == 'X' ||
            (d >= 'a' && d <= 'f') || (d >= 'A' && d <= 'F')) { ++j; continue; }
        if ((d == '+' || d == '-') && j > i && (p[j - 1] == 'e' || p[j - 1] == 'E')) { ++j; continue; }
        break;
      }
      std::string lit(p + i, j - i);
      t.num = std::strtod(lit.c_str(), nullptr);
      t.t = Tok::NUM;
      i = j;
      return t;
    }
    if (c == '"' || c == '\'') {
      const char q = c;
      size_t j = i + 1;
      std::string s;
      while (j < n && p[j] != q) {
        if (p[j] == '\\' && j + 1 < n) {
          const char e = p[j + 1];
          s.push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e);
          j += 2;
        } else {
          s.push_back(p[j]);
          ++j;
        }
      }
      t.v = s;
      t.t = Tok::STR;
      i = j + 1;
      return t;
    }
    if (i + 1 < n) {
      const char d = p[i + 1];
      if ((c == '=' && d == '=') || (c == '~' && d == '=') || (c == '<' && d == '=') ||
          (c == '>' && d == '=') || (c == '.' && d == '.')) {
        t.v.assign(p + i, 2);
        t.t = Tok::OP;
        i += 2;
        return t;
      }
    }
    static const char* ops = "+-*/%#<>=(){}[];,.:";
    if (std::strchr(ops, c)) {
      t.v.assign(1, c);
      t.t = Tok::OP;
      ++i;
      return t;
    }
    err = std::string("非法字符 '") + c + "'";
    t.t = Tok::END;
    i = n;
    return t;
  }
};

// ══════════════ 语法 ══════════════
struct Parser {
  std::vector<Tok> toks;
  size_t p = 0;
  std::string err;
  std::vector<Node*> pool;

  Node* mk(Node::K k) { Node* x = new Node(k); pool.push_back(x); return x; }
  Tok& cur() { return toks[p]; }
  Tok& at(size_t k) { return toks[p + k < toks.size() ? p + k : toks.size() - 1]; }
  bool isOp(const char* v) { return cur().t == Tok::OP && cur().v == v; }
  bool isKw(const char* v) { return cur().t == Tok::KW && cur().v == v; }
  bool eatOp(const char* v) { if (isOp(v)) { ++p; return true; } return false; }
  bool eatKw(const char* v) { if (isKw(v)) { ++p; return true; } return false; }
  bool needOp(const char* v) { if (eatOp(v)) return true; err = std::string("期望 '") + v + "'"; return false; }

  bool block(std::vector<Node*>* out, const char* e1 = "end", const char* e2 = nullptr,
             const char* e3 = nullptr) {
    for (;;) {
      if (cur().t == Tok::END) { err = "脚本提前结束（缺 end）"; return false; }
      if (cur().t == Tok::KW) {
        const std::string& v = cur().v;
        if (v == e1 || (e2 && v == e2) || (e3 && v == e3)) return true;
      }
      Node* st = nullptr;
      if (!statement(&st)) return false;
      out->push_back(st);
    }
  }

  bool funcbody(std::vector<std::string>* params, std::vector<Node*>* body) {
    if (!needOp("(")) return false;
    if (!isOp(")")) {
      do {
        if (cur().t != Tok::NAME) { err = "参数名非法"; return false; }
        params->push_back(cur().v);
        ++p;
      } while (eatOp(","));
    }
    if (!needOp(")")) return false;
    if (!block(body)) return false;
    if (!eatKw("end")) { err = "函数缺 end"; return false; }
    return true;
  }

  bool statement(Node** out) {
    if (cur().t == Tok::KW) {
      const std::string kw = cur().v;
      if (kw == "local") {
        ++p;
        if (isKw("function")) {
          ++p;
          if (cur().t != Tok::NAME) { err = "local function 缺名字"; return false; }
          Node* st = mk(Node::S_LOCALFUNC);
          st->str = cur().v;
          ++p;
          Node* fn = mk(Node::N_FUNC);
          std::vector<Node*> body;
          if (!funcbody(&fn->names, &body)) return false;
          fn->blocks.push_back(body);
          st->kids.push_back(fn);
          *out = st;
          return true;
        }
        Node* st = mk(Node::S_LOCAL);
        do {
          if (cur().t != Tok::NAME) { err = "local 名字非法"; return false; }
          st->names.push_back(cur().v);
          ++p;
        } while (eatOp(","));
        if (eatOp("=")) {
          do {
            Node* e = nullptr;
            if (!expression(&e, 0)) return false;
            st->kids.push_back(e);
          } while (eatOp(","));
        }
        *out = st;
        return true;
      }
      if (kw == "function") {
        ++p;
        if (cur().t != Tok::NAME) { err = "function 缺名字"; return false; }
        Node* st = mk(Node::S_FUNCDECL);
        st->names.push_back(cur().v);
        ++p;
        while (eatOp(".")) {
          if (cur().t != Tok::NAME) { err = "函数名路径非法"; return false; }
          st->names.push_back(cur().v);
          ++p;
        }
        if (eatOp(":")) {
          if (cur().t != Tok::NAME) { err = "方法名非法"; return false; }
          st->str = cur().v;   // 方法名（会自动加 self）
          ++p;
        }
        Node* fn = mk(Node::N_FUNC);
        std::vector<Node*> body;
        if (!funcbody(&fn->names, &body)) return false;
        if (!st->str.empty()) fn->names.insert(fn->names.begin(), "self");
        fn->blocks.push_back(body);
        st->kids.push_back(fn);
        *out = st;
        return true;
      }
      if (kw == "if") {
        ++p;
        Node* st = mk(Node::S_IF);
        for (;;) {
          Node* cond = nullptr;
          if (!expression(&cond, 0)) return false;
          if (!eatKw("then")) { err = "if 缺 then"; return false; }
          std::vector<Node*> body;
          if (!block(&body, "elseif", "else", "end")) return false;
          st->kids.push_back(cond);
          st->blocks.push_back(body);
          if (eatKw("elseif")) continue;
          break;
        }
        if (eatKw("else")) {
          std::vector<Node*> els;
          if (!block(&els, "end")) return false;
          st->kids.push_back(nullptr);   // 占位：表示有 else
          st->blocks.push_back(els);
        }
        if (!eatKw("end")) { err = "if 缺 end"; return false; }
        *out = st;
        return true;
      }
      if (kw == "while") {
        ++p;
        Node* st = mk(Node::S_WHILE);
        Node* cond = nullptr;
        if (!expression(&cond, 0)) return false;
        if (!eatKw("do")) { err = "while 缺 do"; return false; }
        std::vector<Node*> body;
        if (!block(&body, "end")) return false;
        if (!eatKw("end")) { err = "while 缺 end"; return false; }
        st->kids.push_back(cond);
        st->blocks.push_back(body);
        *out = st;
        return true;
      }
      if (kw == "for") {
        ++p;
        if (cur().t != Tok::NAME) { err = "for 缺变量名"; return false; }
        Node* st = nullptr;
        std::string first = cur().v;
        ++p;
        if (eatOp("=")) {
          st = mk(Node::S_FORNUM);
          st->str = first;
          Node* from = nullptr;
          if (!expression(&from, 0)) return false;
          if (!needOp(",")) return false;
          Node* to = nullptr;
          if (!expression(&to, 0)) return false;
          Node* step = nullptr;
          if (eatOp(",")) { if (!expression(&step, 0)) return false; }
          if (!eatKw("do")) { err = "for 缺 do"; return false; }
          std::vector<Node*> body;
          if (!block(&body, "end")) return false;
          if (!eatKw("end")) { err = "for 缺 end"; return false; }
          st->kids.push_back(from);
          st->kids.push_back(to);
          st->kids.push_back(step);
          st->blocks.push_back(body);
          *out = st;
          return true;
        }
        st = mk(Node::S_FORIN);
        st->names.push_back(first);
        while (eatOp(",")) {
          if (cur().t != Tok::NAME) { err = "for-in 变量名非法"; return false; }
          st->names.push_back(cur().v);
          ++p;
        }
        if (!eatKw("in")) { err = "for 缺 in"; return false; }
        do {
          Node* e = nullptr;
          if (!expression(&e, 0)) return false;
          st->kids.push_back(e);
        } while (eatOp(","));
        if (!eatKw("do")) { err = "for 缺 do"; return false; }
        std::vector<Node*> body;
        if (!block(&body, "end")) return false;
        if (!eatKw("end")) { err = "for 缺 end"; return false; }
        st->blocks.push_back(body);
        *out = st;
        return true;
      }
      if (kw == "return") {
        ++p;
        Node* st = mk(Node::S_RETURN);
        if (!isKw("end") && !isKw("else") && !isKw("elseif") && cur().t != Tok::END) {
          do {
            Node* e = nullptr;
            if (!expression(&e, 0)) return false;
            st->kids.push_back(e);
          } while (eatOp(","));
        }
        *out = st;
        return true;
      }
      if (kw == "break") { ++p; *out = mk(Node::S_BREAK); return true; }
      if (kw == "do") {
        ++p;
        Node* st = mk(Node::S_DO);
        std::vector<Node*> body;
        if (!block(&body, "end")) return false;
        if (!eatKw("end")) { err = "do 缺 end"; return false; }
        st->blocks.push_back(body);
        *out = st;
        return true;
      }
    }
    // 赋值 或 调用语句
    Node* e = nullptr;
    if (!suffixed(&e)) return false;
    if (isOp("=") || isOp(",")) {
      Node* st = mk(Node::S_ASSIGN);
      st->kids.push_back(e);
      while (eatOp(",")) {
        Node* t2 = nullptr;
        if (!suffixed(&t2)) return false;
        st->kids.push_back(t2);
      }
      if (!needOp("=")) return false;
      size_t ntargets = st->kids.size();
      do {
        Node* v = nullptr;
        if (!expression(&v, 0)) return false;
        st->kids.push_back(v);
      } while (eatOp(","));
      (void)ntargets;
      *out = st;
      return true;
    }
    Node* st = mk(Node::S_CALLSTMT);
    st->kids.push_back(e);
    *out = st;
    return true;
  }

  bool primary(Node** out) {
    if (cur().t == Tok::NUM) {
      Node* n = mk(Node::N_NUM);
      n->num = cur().num;
      ++p;
      *out = n;
      return true;
    }
    if (cur().t == Tok::STR) {
      Node* n = mk(Node::N_STR);
      n->str = cur().v;
      ++p;
      *out = n;
      return true;
    }
    if (cur().t == Tok::KW) {
      if (cur().v == "nil") { ++p; *out = mk(Node::N_NIL); return true; }
      if (cur().v == "true" || cur().v == "false") {
        Node* n = mk(Node::N_BOOL);
        n->b = cur().v == "true";
        ++p;
        *out = n;
        return true;
      }
      if (cur().v == "function") {
        ++p;
        Node* fn = mk(Node::N_FUNC);
        std::vector<Node*> body;
        if (!funcbody(&fn->names, &body)) return false;
        fn->blocks.push_back(body);
        *out = fn;
        return true;
      }
    }
    if (isOp("{")) {
      ++p;
      Node* n = mk(Node::N_TABLE);
      while (!isOp("}")) {
        if (isOp("[")) {
          ++p;
          Node* key = nullptr;
          if (!expression(&key, 0)) return false;
          if (!needOp("]")) return false;
          if (!needOp("=")) return false;
          Node* val = nullptr;
          if (!expression(&val, 0)) return false;
          n->kids.push_back(key);
          n->kids.push_back(val);
        } else if (cur().t == Tok::NAME && at(1).t == Tok::OP && at(1).v == "=") {
          Node* key = mk(Node::N_STR);
          key->str = cur().v;
          ++p;
          ++p;
          Node* val = nullptr;
          if (!expression(&val, 0)) return false;
          n->kids.push_back(key);
          n->kids.push_back(val);
        } else {
          Node* val = nullptr;
          if (!expression(&val, 0)) return false;
          n->kids.push_back(nullptr);
          n->kids.push_back(val);
        }
        if (!eatOp(",")) break;
      }
      if (!needOp("}")) return false;
      *out = n;
      return true;
    }
    if (isOp("(")) {
      ++p;
      if (!expression(out, 0)) return false;
      if (!needOp(")")) return false;
      return true;
    }
    if (cur().t == Tok::NAME) {
      Node* n = mk(Node::N_NAME);
      n->str = cur().v;
      ++p;
      *out = n;
      return true;
    }
    err = "意外的记号 '" + cur().v + "'";
    return false;
  }

  bool callargs(std::vector<Node*>* args) {
    if (isOp("(")) {
      ++p;
      if (!isOp(")")) {
        do {
          Node* a = nullptr;
          if (!expression(&a, 0)) return false;
          args->push_back(a);
        } while (eatOp(","));
      }
      if (!needOp(")")) return false;
      return true;
    }
    if (isOp("{")) {
      Node* t = nullptr;
      if (!primary(&t)) return false;
      args->push_back(t);
      return true;
    }
    if (cur().t == Tok::STR) {
      Node* s = mk(Node::N_STR);
      s->str = cur().v;
      ++p;
      args->push_back(s);
      return true;
    }
    return true;   // 无参调用（极少见，容忍）
  }

  bool suffixed(Node** out) {
    Node* e = nullptr;
    if (!primary(&e)) return false;
    for (;;) {
      if (eatOp(".")) {
        if (cur().t != Tok::NAME) { err = "字段名非法"; return false; }
        Node* idx = mk(Node::N_INDEX);
        idx->kids.push_back(e);
        Node* key = mk(Node::N_STR);
        key->str = cur().v;
        ++p;
        idx->kids.push_back(key);
        e = idx;
        continue;
      }
      if (eatOp("[")) {
        Node* key = nullptr;
        if (!expression(&key, 0)) return false;
        if (!needOp("]")) return false;
        Node* idx = mk(Node::N_INDEX);
        idx->kids.push_back(e);
        idx->kids.push_back(key);
        e = idx;
        continue;
      }
      if (eatOp(":")) {
        if (cur().t != Tok::NAME) { err = "方法名非法"; return false; }
        Node* m = mk(Node::N_METHOD);
        m->str = cur().v;
        ++p;
        m->kids.push_back(e);
        std::vector<Node*> args;
        if (!callargs(&args)) return false;
        for (Node* a : args) m->kids.push_back(a);
        e = m;
        continue;
      }
      if (isOp("(") || isOp("{") || cur().t == Tok::STR) {
        Node* c = mk(Node::N_CALL);
        c->kids.push_back(e);
        std::vector<Node*> args;
        if (!callargs(&args)) return false;
        for (Node* a : args) c->kids.push_back(a);
        e = c;
        continue;
      }
      break;
    }
    *out = e;
    return true;
  }

  int binpri(const std::string& op) {
    if (op == "or") return 1;
    if (op == "and") return 2;
    if (op == "<" || op == ">" || op == "<=" || op == ">=" || op == "~=" || op == "==") return 3;
    if (op == "..") return 4;
    if (op == "+" || op == "-") return 5;
    if (op == "*" || op == "/" || op == "%") return 6;
    return -1;
  }

  bool expression(Node** out, int limit) {
    Node* left = nullptr;
    if (isKw("not") || isOp("-") || isOp("#")) {
      std::string op = cur().v;
      ++p;
      Node* e = nullptr;
      if (!expression(&e, 7)) return false;
      Node* un = mk(Node::N_UN);
      un->str = op;
      un->kids.push_back(e);
      left = un;
    } else {
      if (!suffixed(&left)) return false;
    }
    for (;;) {
      const bool isKwOp = cur().t == Tok::KW && (cur().v == "and" || cur().v == "or");
      const bool isBin = isKwOp || (cur().t == Tok::OP && binpri(cur().v) > 0);
      if (!isBin) break;
      const int pri = binpri(cur().v);
      if (pri <= limit) break;
      const std::string op = cur().v;
      ++p;
      Node* right = nullptr;
      if (!expression(&right, (op == "..") ? pri - 1 : pri)) return false;
      Node* bin = mk(Node::N_BIN);
      bin->str = op;
      bin->kids.push_back(left);
      bin->kids.push_back(right);
      left = bin;
    }
    *out = left;
    return true;
  }
};

// ══════════════ 环境 / VM ══════════════
struct Env {
  std::map<std::string, Value> vars;
  std::shared_ptr<Env> parent;
};

struct VM {
  std::shared_ptr<Env> globals;
  std::vector<Node*> pool;
  std::vector<Node*> ast;
  std::string err;
  std::string name;
  int depth = 0;
  long long budget = 20000000;
};

enum Stat { ST_OK = 0, ST_BREAK = 1, ST_RETURN = 2, ST_ERR = 3 };

void setErr(VM* vm, const std::string& m) { if (vm->err.empty()) vm->err = m; }

// ══════════════ 求值 ══════════════
Stat evalExpr(VM* vm, Env* env, Node* n, Value* out);
Stat execBlock(VM* vm, Env* env, const std::vector<Node*>& body, Value* ret);

Value makeNum(double d) { Value v; v.t = Value::NUM; v.n = d; return v; }
Value makeStr(const std::string& s) { Value v; v.t = Value::STR; v.s = s; return v; }
Value makeBool(bool b) { Value v; v.t = Value::BOOL; v.b = b; return v; }
Value makeNil() { return Value(); }

bool truthy(const Value& v) {
  if (v.t == Value::NIL) return false;
  if (v.t == Value::BOOL) return v.b;
  return true;
}

std::string toStr(const Value& v) {
  switch (v.t) {
    case Value::NIL: return "nil";
    case Value::BOOL: return v.b ? "true" : "false";
    case Value::NUM: {
      char buf[40];
      if (v.n == std::floor(v.n) && std::fabs(v.n) < 1e15) std::snprintf(buf, sizeof(buf), "%.0f", v.n);
      else std::snprintf(buf, sizeof(buf), "%.14g", v.n);
      return std::string(buf);
    }
    case Value::STR: return v.s;
    case Value::TAB: return "table";
    case Value::FN: return "function";
  }
  return "";
}

bool toNum(const Value& v, double* out) {
  if (v.t == Value::NUM) { *out = v.n; return true; }
  if (v.t == Value::STR) {
    char* end = nullptr;
    const double d = std::strtod(v.s.c_str(), &end);
    if (end && *end == 0 && end != v.s.c_str()) { *out = d; return true; }
  }
  return false;
}

Value tableGet(const std::shared_ptr<Table>& t, const std::string& key) {
  if (!t) return makeNil();
  auto it = t->map.find(key);
  if (it == t->map.end()) return makeNil();
  return it->second;
}

void tableSet(const std::shared_ptr<Table>& t, const std::string& key, const Value& v) {
  if (!t) return;
  if (t->map.find(key) == t->map.end()) t->order.push_back(key);
  t->map[key] = v;
}

// ══════════════ JSON ══════════════
void jsonEncode(const Value& v, std::string* out, int depth = 0) {
  if (depth > 16) return;
  switch (v.t) {
    case Value::NIL: out->append("null"); return;
    case Value::BOOL: out->append(v.b ? "true" : "false"); return;
    case Value::NUM: {
      char buf[40];
      if (v.n == std::floor(v.n) && std::fabs(v.n) < 1e15) std::snprintf(buf, sizeof(buf), "%.0f", v.n);
      else std::snprintf(buf, sizeof(buf), "%.14g", v.n);
      out->append(buf);
      return;
    }
    case Value::STR: {
      out->push_back('"');
      for (char c : v.s) {
        switch (c) {
          case '"': out->append("\\\""); break;
          case '\\': out->append("\\\\"); break;
          case '\n': out->append("\\n"); break;
          case '\r': out->append("\\r"); break;
          case '\t': out->append("\\t"); break;
          default:
            if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out->append(b); }
            else out->push_back(c);
        }
      }
      out->push_back('"');
      return;
    }
    case Value::TAB: {
      if (!v.tab) { out->append("null"); return; }
      // 判断是否纯数组（1..n）
      const size_t n = v.tab->map.size();
      bool isArr = true;
      for (size_t i = 1; i <= n; ++i) {
        if (v.tab->map.find(keyOfNum((double)i)) == v.tab->map.end()) { isArr = false; break; }
      }
      out->push_back(isArr ? '[' : '{');
      bool first = true;
      if (isArr) {
        for (size_t i = 1; i <= n; ++i) {
          if (!first) out->push_back(',');
          first = false;
          jsonEncode(v.tab->map[keyOfNum((double)i)], out, depth + 1);
        }
      } else {
        for (const auto& kv : v.tab->map) {
          if (!first) out->push_back(',');
          first = false;
          Value k; k.t = Value::STR;
          if (kv.first.size() > 2 && kv.first[0] == 's') k.s = kv.first.substr(2);
          else k.s = kv.first;
          jsonEncode(k, out, depth + 1);
          out->push_back(':');
          jsonEncode(kv.second, out, depth + 1);
        }
      }
      out->push_back(isArr ? ']' : '}');
      return;
    }
    case Value::FN: out->append("null"); return;
  }
}

struct JsonP {
  const char* p;
  const char* end;
  std::string err;
  void skip() {
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
  }
  bool parse(Value* out) {
    skip();
    if (p >= end) { err = "JSON 提前结束"; return false; }
    const char c = *p;
    if (c == '{') return parseObj(out);
    if (c == '[') return parseArr(out);
    if (c == '"') return parseStr(out);
    if (c == 't' && end - p >= 4 && std::strncmp(p, "true", 4) == 0) { p += 4; *out = makeBool(true); return true; }
    if (c == 'f' && end - p >= 5 && std::strncmp(p, "false", 5) == 0) { p += 5; *out = makeBool(false); return true; }
    if (c == 'n' && end - p >= 4 && std::strncmp(p, "null", 4) == 0) { p += 4; *out = makeNil(); return true; }
    char* e = nullptr;
    const double d = std::strtod(p, &e);
    if (e && e != p) { p = e; *out = makeNum(d); return true; }
    err = "JSON 非法值";
    return false;
  }
  bool parseStr(Value* out) {
    if (*p != '"') { err = "JSON 期望字符串"; return false; }
    ++p;
    std::string s;
    while (p < end && *p != '"') {
      if (*p == '\\' && p + 1 < end) {
        const char e = p[1];
        if (e == 'n') s.push_back('\n');
        else if (e == 't') s.push_back('\t');
        else if (e == 'r') s.push_back('\r');
        else if (e == 'u') {
          unsigned cp = 0;
          for (int k = 0; k < 4 && p + 2 + k < end; ++k) {
            const char h = p[2 + k];
            cp = cp * 16 + (unsigned)(h >= '0' && h <= '9' ? h - '0' : (h >= 'a' && h <= 'f' ? h - 'a' + 10 : h - 'A' + 10));
          }
          if (cp < 0x80) s.push_back((char)cp);
          else if (cp < 0x800) { s.push_back((char)(0xC0 | (cp >> 6))); s.push_back((char)(0x80 | (cp & 0x3F))); }
          else { s.push_back((char)(0xE0 | (cp >> 12))); s.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); s.push_back((char)(0x80 | (cp & 0x3F))); }
          p += 6;
          continue;
        }
        else s.push_back(e);
        p += 2;
      } else {
        s.push_back(*p);
        ++p;
      }
    }
    if (p < end) ++p;
    *out = makeStr(s);
    return true;
  }
  bool parseArr(Value* out) {
    ++p;   // [
    auto t = std::make_shared<Table>();
    double idx = 1;
    skip();
    if (p < end && *p == ']') { ++p; Value v; v.t = Value::TAB; v.tab = t; *out = v; return true; }
    for (;;) {
      Value item;
      if (!parse(&item)) return false;
      tableSet(t, keyOfNum(idx), item);
      ++idx;
      skip();
      if (p < end && *p == ',') { ++p; continue; }
      if (p < end && *p == ']') { ++p; break; }
      err = "JSON 数组非法";
      return false;
    }
    Value v; v.t = Value::TAB; v.tab = t; *out = v;
    return true;
  }
  bool parseObj(Value* out) {
    ++p;   // {
    auto t = std::make_shared<Table>();
    skip();
    if (p < end && *p == '}') { ++p; Value v; v.t = Value::TAB; v.tab = t; *out = v; return true; }
    for (;;) {
      Value k;
      skip();
      if (!parseStr(&k)) return false;
      skip();
      if (p >= end || *p != ':') { err = "JSON 对象缺 :"; return false; }
      ++p;
      Value val;
      if (!parse(&val)) return false;
      tableSet(t, keyOfStr(k.s), val);
      skip();
      if (p < end && *p == ',') { ++p; continue; }
      if (p < end && *p == '}') { ++p; break; }
      err = "JSON 对象非法";
      return false;
    }
    Value v; v.t = Value::TAB; v.tab = t; *out = v;
    return true;
  }
};

// ══════════════ 宿主函数 ══════════════
enum HostId {
  H_HTTP_GET = 0, H_HTTP_POST, H_LOG, H_TOSTRING, H_TONUMBER, H_TYPE, H_PCALL, H_IPAIRS,
  H_JSON_ENCODE, H_JSON_DECODE, H_TABLE_INSERT, H_TABLE_CONCAT, H_MATH_FLOOR, H_OS_TIME,
};

Stat callHost(VM* vm, int host, const std::vector<Value>& args, std::vector<Value>* rets) {
  switch (host) {
    case H_HTTP_GET:
    case H_HTTP_POST: {
      const std::string url = args.size() > 0 ? toStr(args[0]) : "";
      const std::string body = args.size() > 1 ? toStr(args[1]) : "";
      int len = 0, status = 0;
      char* resp = yieyu_fetch_ex(url.c_str(), host == H_HTTP_POST ? "POST" : "GET", body.c_str(), "", &len, &status);
      std::string s;
      if (resp) {
        s.assign(resp, len > 0 ? (size_t)len : std::strlen(resp));
        yieyu_fetch_free(resp);
      }
      rets->push_back(makeStr(s));
      rets->push_back(makeNum((double)status));
      return ST_OK;
    }
    case H_LOG: return ST_OK;   // 成功不记日志
    case H_TOSTRING: rets->push_back(makeStr(args.empty() ? "nil" : toStr(args[0]))); return ST_OK;
    case H_TONUMBER: {
      double d = 0;
      if (!args.empty() && toNum(args[0], &d)) rets->push_back(makeNum(d));
      else rets->push_back(makeNil());
      return ST_OK;
    }
    case H_TYPE: {
      const char* t = "nil";
      if (!args.empty()) {
        switch (args[0].t) {
          case Value::NIL: t = "nil"; break;
          case Value::BOOL: t = "boolean"; break;
          case Value::NUM: t = "number"; break;
          case Value::STR: t = "string"; break;
          case Value::TAB: t = "table"; break;
          case Value::FN: t = "function"; break;
        }
      }
      rets->push_back(makeStr(t));
      return ST_OK;
    }
    case H_PCALL: {
      // pcall(f, ...) → (true, 返回值) 或 (false, 错误串)
      if (args.empty()) { rets->push_back(makeBool(false)); rets->push_back(makeStr("pcall 缺函数")); return ST_OK; }
      std::vector<Value> sub(args.begin() + 1, args.end());
      std::vector<Value> inner;
      const std::string saved = vm->err;
      vm->err.clear();
      const Stat st = callValue(vm, vm->globals.get(), args[0], sub, &inner);
      if (st == ST_ERR) {
        rets->push_back(makeBool(false));
        rets->push_back(makeStr(vm->err));
        vm->err.clear();
        return ST_OK;
      }
      vm->err = saved;
      rets->push_back(makeBool(true));
      rets->push_back(inner.empty() ? makeNil() : inner[0]);
      return ST_OK;
    }
    case H_IPAIRS: {
      if (args.size() < 2) {
        // ipairs(t) → (迭代器, 表, 0)
        Value it; it.t = Value::FN; it.host = H_IPAIRS;
        rets->push_back(it);
        rets->push_back(args.empty() ? makeNil() : args[0]);
        rets->push_back(makeNum(0));
        return ST_OK;
      }
      // 迭代器调用：ipairs(表, k) → (表[k], k)；越界返回 nil
      const Value& tab = args[0];
      double k = 0;
      toNum(args[1], &k);
      if (tab.t != Value::TAB || !tab.tab) { rets->push_back(makeNil()); return ST_OK; }
      auto it2 = tab.tab->map.find(keyOfNum(k));
      if (it2 == tab.tab->map.end()) { rets->push_back(makeNil()); return ST_OK; }
      rets->push_back(it2->second);
      rets->push_back(makeNum(k));
      return ST_OK;
    }
    case H_JSON_ENCODE: {
      std::string s;
      jsonEncode(args.empty() ? makeNil() : args[0], &s);
      rets->push_back(makeStr(s));
      return ST_OK;
    }
    case H_JSON_DECODE: {
      const std::string src = args.empty() ? "" : toStr(args[0]);
      JsonP jp{src.c_str(), src.c_str() + src.size(), {}};
      Value v;
      if (!jp.parse(&v)) { rets->push_back(makeNil()); return ST_OK; }
      rets->push_back(v);
      return ST_OK;
    }
    case H_TABLE_INSERT: {
      if (args.size() >= 2 && args[0].t == Value::TAB && args[0].tab) {
        double n = 1;
        while (args[0].tab->map.find(keyOfNum(n)) != args[0].tab->map.end()) ++n;
        tableSet(args[0].tab, keyOfNum(n), args[1]);
      }
      return ST_OK;
    }
    case H_TABLE_CONCAT: {
      std::string out;
      const std::string sep = args.size() > 1 ? toStr(args[1]) : "";
      if (!args.empty() && args[0].t == Value::TAB && args[0].tab) {
        for (double i = 1;; ++i) {
          auto it = args[0].tab->map.find(keyOfNum(i));
          if (it == args[0].tab->map.end()) break;
          if (i > 1) out += sep;
          out += toStr(it->second);
        }
      }
      rets->push_back(makeStr(out));
      return ST_OK;
    }
    case H_MATH_FLOOR: {
      double d = 0;
      if (!args.empty() && toNum(args[0], &d)) rets->push_back(makeNum(std::floor(d)));
      else rets->push_back(makeNil());
      return ST_OK;
    }
    case H_OS_TIME: rets->push_back(makeNum((double)std::time(nullptr))); return ST_OK;
  }
  return ST_ERR;
}

void installGlobals(VM* vm) {
  auto setHost = [&](const char* name, int id) {
    Value f; f.t = Value::FN; f.host = id;
    vm->globals->vars[name] = f;
  };
  setHost("http_get", H_HTTP_GET);
  setHost("http_post", H_HTTP_POST);
  setHost("request", H_HTTP_GET);
  setHost("fetch", H_HTTP_GET);
  setHost("log", H_LOG);
  setHost("print", H_LOG);
  setHost("tostring", H_TOSTRING);
  setHost("tonumber", H_TONUMBER);
  setHost("type", H_TYPE);
  setHost("pcall", H_PCALL);
  setHost("ipairs", H_IPAIRS);
  setHost("pairs", H_IPAIRS);
  // 表库
  {
    Value t; t.t = Value::TAB; t.tab = std::make_shared<Table>();
    Value ins; ins.t = Value::FN; ins.host = H_TABLE_INSERT;
    Value cat; cat.t = Value::FN; cat.host = H_TABLE_CONCAT;
    tableSet(t.tab, keyOfStr("insert"), ins);
    tableSet(t.tab, keyOfStr("concat"), cat);
    vm->globals->vars["table"] = t;
  }
  {
    Value t; t.t = Value::TAB; t.tab = std::make_shared<Table>();
    Value fl; fl.t = Value::FN; fl.host = H_MATH_FLOOR;
    tableSet(t.tab, keyOfStr("floor"), fl);
    vm->globals->vars["math"] = t;
  }
  {
    Value t; t.t = Value::TAB; t.tab = std::make_shared<Table>();
    Value en; en.t = Value::FN; en.host = H_JSON_ENCODE;
    Value de; de.t = Value::FN; de.host = H_JSON_DECODE;
    tableSet(t.tab, keyOfStr("encode"), en);
    tableSet(t.tab, keyOfStr("decode"), de);
    vm->globals->vars["json"] = t;
  }
  {
    Value t; t.t = Value::TAB; t.tab = std::make_shared<Table>();
    Value tm; tm.t = Value::FN; tm.host = H_OS_TIME;
    tableSet(t.tab, keyOfStr("time"), tm);
    vm->globals->vars["os"] = t;
  }
}

// 查找变量（沿环境链）
bool lookup(Env* env, const std::string& name, Value* out) {
  for (Env* e = env; e; e = e->parent.get()) {
    auto it = e->vars.find(name);
    if (it != e->vars.end()) { *out = it->second; return true; }
  }
  return false;
}

Stat execBlock(VM* vm, Env* env, const std::vector<Node*>& body, Value* ret);

Stat callValue(VM* vm, Env* env, const Value& fn, std::vector<Value> args, std::vector<Value>* rets);

}  // namespace

namespace {

Stat evalExprMulti(VM* vm, Env* env, Node* n, std::vector<Value>* out);

bool spend(VM* vm) {
  if (--vm->budget <= 0) { setErr(vm, "脚本指令数超限（疑似死循环）"); return false; }
  return true;
}

Stat evalExpr(VM* vm, Env* env, Node* n, Value* out) {
  std::vector<Value> vs;
  const Stat st = evalExprMulti(vm, env, n, &vs);
  *out = vs.empty() ? makeNil() : vs[0];
  return st;
}

Stat callValue(VM* vm, Env* env, const Value& fn, std::vector<Value> args, std::vector<Value>* rets) {
  if (fn.t == Value::FN && fn.host >= 0) return callHost(vm, fn.host, args, rets);
  if (fn.t != Value::FN || !fn.fn) {
    setErr(vm, "试图调用非函数（" + toStr(fn) + "）");
    return ST_ERR;
  }
  if (vm->depth >= 32) { setErr(vm, "调用层级过深"); return ST_ERR; }
  ++vm->depth;
  auto frame = std::make_shared<Env>();
  frame->parent = fn.fn->closure;
  for (size_t i = 0; i < fn.fn->params.size(); ++i) {
    frame->vars[fn.fn->params[i]] = i < args.size() ? args[i] : makeNil();
  }
  Value ret;
  const Stat st = execBlock(vm, frame.get(), fn.fn->body, &ret);
  --vm->depth;
  if (st == ST_ERR) return ST_ERR;
  rets->push_back(ret);
  return ST_OK;
}

// 取索引（表或字符串）
Value indexValue(const Value& obj, const Value& key) {
  if (obj.t == Value::TAB && obj.tab) {
    if (key.t == Value::NUM) return tableGet(obj.tab, keyOfNum(key.n));
    if (key.t == Value::STR) return tableGet(obj.tab, keyOfStr(key.s));
    return makeNil();
  }
  return makeNil();
}

Stat assignTo(VM* vm, Env* env, Node* target, const Value& v) {
  if (target->k == Node::N_NAME) {
    // 找到已存在的绑定就写它，否则写当前作用域
    for (Env* e = env; e; e = e->parent.get()) {
      auto it = e->vars.find(target->str);
      if (it != e->vars.end()) { it->second = v; return ST_OK; }
    }
    env->vars[target->str] = v;
    return ST_OK;
  }
  if (target->k == Node::N_INDEX) {
    Value obj, key;
    if (evalExpr(vm, env, target->kids[0], &obj) != ST_OK) return ST_ERR;
    if (evalExpr(vm, env, target->kids[1], &key) != ST_OK) return ST_ERR;
    if (obj.t != Value::TAB || !obj.tab) { setErr(vm, "给非表赋值"); return ST_ERR; }
    tableSet(obj.tab, key.t == Value::NUM ? keyOfNum(key.n) : keyOfStr(toStr(key)), v);
    return ST_OK;
  }
  setErr(vm, "非法的赋值目标");
  return ST_ERR;
}

Stat evalExprMulti(VM* vm, Env* env, Node* n, std::vector<Value>* out) {
  if (!n) { out->push_back(makeNil()); return ST_OK; }
  if (!spend(vm)) return ST_ERR;
  out->clear();
  switch (n->k) {
    case Node::N_NUM: out->push_back(makeNum(n->num)); return ST_OK;
    case Node::N_STR: out->push_back(makeStr(n->str)); return ST_OK;
    case Node::N_NIL: out->push_back(makeNil()); return ST_OK;
    case Node::N_BOOL: out->push_back(makeBool(n->b)); return ST_OK;
    case Node::N_NAME: {
      Value v;
      if (!lookup(env, n->str, &v)) v = makeNil();
      out->push_back(v);
      return ST_OK;
    }
    case Node::N_FUNC: {
      auto f = std::make_shared<Func>();
      f->params = n->names;
      f->body = n->blocks.empty() ? std::vector<Node*>() : n->blocks[0];
      f->closure = std::make_shared<Env>();
      // 闭包捕获：把当前可见变量浅拷贝一份（简化实现，够脚本用）
      for (Env* e = env; e; e = e->parent.get()) {
        for (const auto& kv : e->vars) if (f->closure->vars.find(kv.first) == f->closure->vars.end()) f->closure->vars[kv.first] = kv.second;
      }
      Value v; v.t = Value::FN; v.fn = f;
      out->push_back(v);
      return ST_OK;
    }
    case Node::N_TABLE: {
      auto t = std::make_shared<Table>();
      double nextIdx = 1;
      for (size_t i = 0; i + 1 < n->kids.size(); i += 2) {
        Node* key = n->kids[i];
        Node* val = n->kids[i + 1];
        Value v;
        if (evalExpr(vm, env, val, &v) != ST_OK) return ST_ERR;
        if (!key) {
          tableSet(t, keyOfNum(nextIdx++), v);
        } else {
          Value k;
          if (evalExpr(vm, env, key, &k) != ST_OK) return ST_ERR;
          tableSet(t, k.t == Value::NUM ? keyOfNum(k.n) : keyOfStr(toStr(k)), v);
        }
      }
      Value v; v.t = Value::TAB; v.tab = t;
      out->push_back(v);
      return ST_OK;
    }
    case Node::N_INDEX: {
      Value obj, key;
      if (evalExpr(vm, env, n->kids[0], &obj) != ST_OK) return ST_ERR;
      if (evalExpr(vm, env, n->kids[1], &key) != ST_OK) return ST_ERR;
      out->push_back(indexValue(obj, key));
      return ST_OK;
    }
    case Node::N_CALL:
    case Node::N_METHOD: {
      Value fn;
      std::vector<Value> args;
      if (n->k == Node::N_CALL) {
        if (evalExpr(vm, env, n->kids[0], &fn) != ST_OK) return ST_ERR;
        for (size_t i = 1; i < n->kids.size(); ++i) {
          Value a;
          if (evalExpr(vm, env, n->kids[i], &a) != ST_OK) return ST_ERR;
          args.push_back(a);
        }
      } else {
        Value obj;
        if (evalExpr(vm, env, n->kids[0], &obj) != ST_OK) return ST_ERR;
        fn = indexValue(obj, makeStr(n->str));
        args.push_back(obj);
        for (size_t i = 1; i < n->kids.size(); ++i) {
          Value a;
          if (evalExpr(vm, env, n->kids[i], &a) != ST_OK) return ST_ERR;
          args.push_back(a);
        }
      }
      std::vector<Value> rets;
      if (callValue(vm, env, fn, args, &rets) != ST_OK) return ST_ERR;
      if (rets.empty()) out->push_back(makeNil());
      else *out = rets;
      return ST_OK;
    }
    case Node::N_UN: {
      Value v;
      if (evalExpr(vm, env, n->kids[0], &v) != ST_OK) return ST_ERR;
      if (n->str == "not") { out->push_back(makeBool(!truthy(v))); return ST_OK; }
      if (n->str == "-") {
        double d = 0;
        if (!toNum(v, &d)) { setErr(vm, "取负的不是数字"); return ST_ERR; }
        out->push_back(makeNum(-d));
        return ST_OK;
      }
      if (n->str == "#") {
        double len = 0;
        if (v.t == Value::STR) len = (double)v.s.size();
        else if (v.t == Value::TAB && v.tab) {
          while (v.tab->map.find(keyOfNum(len + 1)) != v.tab->map.end()) len += 1;
        }
        out->push_back(makeNum(len));
        return ST_OK;
      }
      out->push_back(makeNil());
      return ST_OK;
    }
    case Node::N_BIN: {
      const std::string& op = n->str;
      if (op == "and") {
        Value a;
        if (evalExpr(vm, env, n->kids[0], &a) != ST_OK) return ST_ERR;
        if (!truthy(a)) { out->push_back(a); return ST_OK; }
        Value b;
        if (evalExpr(vm, env, n->kids[1], &b) != ST_OK) return ST_ERR;
        out->push_back(b);
        return ST_OK;
      }
      if (op == "or") {
        Value a;
        if (evalExpr(vm, env, n->kids[0], &a) != ST_OK) return ST_ERR;
        if (truthy(a)) { out->push_back(a); return ST_OK; }
        Value b;
        if (evalExpr(vm, env, n->kids[1], &b) != ST_OK) return ST_ERR;
        out->push_back(b);
        return ST_OK;
      }
      Value a, b;
      if (evalExpr(vm, env, n->kids[0], &a) != ST_OK) return ST_ERR;
      if (evalExpr(vm, env, n->kids[1], &b) != ST_OK) return ST_ERR;
      if (op == "..") { out->push_back(makeStr(toStr(a) + toStr(b))); return ST_OK; }
      if (op == "==" || op == "~=") {
        bool eq = false;
        if (a.t == Value::NUM && b.t == Value::NUM) eq = a.n == b.n;
        else if (a.t == Value::STR && b.t == Value::STR) eq = a.s == b.s;
        else if (a.t == Value::BOOL && b.t == Value::BOOL) eq = a.b == b.b;
        else if (a.t == Value::NIL && b.t == Value::NIL) eq = true;
        else if (a.t == Value::TAB && b.t == Value::TAB) eq = a.tab == b.tab;
        else eq = false;
        out->push_back(makeBool(op == "==" ? eq : !eq));
        return ST_OK;
      }
      if (op == "<" || op == ">" || op == "<=" || op == ">=") {
        bool r = false;
        if (a.t == Value::NUM && b.t == Value::NUM) {
          if (op == "<") r = a.n < b.n;
          else if (op == ">") r = a.n > b.n;
          else if (op == "<=") r = a.n <= b.n;
          else r = a.n >= b.n;
        } else if (a.t == Value::STR && b.t == Value::STR) {
          if (op == "<") r = a.s < b.s;
          else if (op == ">") r = a.s > b.s;
          else if (op == "<=") r = a.s <= b.s;
          else r = a.s >= b.s;
        } else {
          setErr(vm, "比较操作数类型不支持");
          return ST_ERR;
        }
        out->push_back(makeBool(r));
        return ST_OK;
      }
      double x = 0, y = 0;
      if (!toNum(a, &x) || !toNum(b, &y)) { setErr(vm, "算术操作数不是数字"); return ST_ERR; }
      if (op == "+") out->push_back(makeNum(x + y));
      else if (op == "-") out->push_back(makeNum(x - y));
      else if (op == "*") out->push_back(makeNum(x * y));
      else if (op == "/") { if (y == 0) { out->push_back(makeNum(0)); } else out->push_back(makeNum(x / y)); }
      else if (op == "%") { if (y == 0) { out->push_back(makeNum(0)); } else out->push_back(makeNum(std::fmod(x, y))); }
      else out->push_back(makeNil());
      return ST_OK;
    }
  }
  out->push_back(makeNil());
  return ST_OK;
}

Stat execBlock(VM* vm, Env* env, const std::vector<Node*>& body, Value* ret) {
  for (Node* st : body) {
    if (!spend(vm)) return ST_ERR;
    switch (st->k) {
      case Node::S_LOCAL: {
        std::vector<Value> vals;
        for (Node* e : st->kids) {
          std::vector<Value> vs;
          if (evalExprMulti(vm, env, e, &vs) != ST_OK) return ST_ERR;
          for (const auto& v : vs) vals.push_back(v);
        }
        for (size_t i = 0; i < st->names.size(); ++i) {
          env->vars[st->names[i]] = i < vals.size() ? vals[i] : makeNil();
        }
        break;
      }
      case Node::S_ASSIGN: {
        const size_t ntargets = st->kids.size() / 2;   // 前一半目标，后一半值
        std::vector<Value> vals;
        for (size_t i = ntargets; i < st->kids.size(); ++i) {
          std::vector<Value> vs;
          if (evalExprMulti(vm, env, st->kids[i], &vs) != ST_OK) return ST_ERR;
          for (const auto& v : vs) vals.push_back(v);
        }
        for (size_t i = 0; i < ntargets; ++i) {
          if (assignTo(vm, env, st->kids[i], i < vals.size() ? vals[i] : makeNil()) != ST_OK) return ST_ERR;
        }
        break;
      }
      case Node::S_CALLSTMT: {
        std::vector<Value> vs;
        if (evalExprMulti(vm, env, st->kids[0], &vs) != ST_OK) return ST_ERR;
        break;
      }
      case Node::S_IF: {
        // kids: cond1, cond2, ..., (nullptr if else present)；blocks 与 kids 一一对应
        size_t bi = 0;
        bool done = false;
        for (size_t i = 0; i < st->kids.size(); ++i) {
          if (!st->kids[i]) {   // else 占位
            const Stat s = execBlock(vm, env, st->blocks[i], ret);
            if (s != ST_OK) return s;
            done = true;
            break;
          }
          Value c;
          if (evalExpr(vm, env, st->kids[i], &c) != ST_OK) return ST_ERR;
          if (truthy(c)) {
            const Stat s = execBlock(vm, env, st->blocks[i], ret);
            if (s != ST_OK) return s;
            done = true;
            break;
          }
          ++bi;
        }
        (void)done;
        break;
      }
      case Node::S_WHILE: {
        for (;;) {
          Value c;
          if (evalExpr(vm, env, st->kids[0], &c) != ST_OK) return ST_ERR;
          if (!truthy(c)) break;
          auto scope = std::make_shared<Env>();
          scope->parent = std::shared_ptr<Env>(env, [](Env*) {});   // 不持有所有权
          const Stat s = execBlock(vm, scope.get(), st->blocks[0], ret);
          if (s == ST_BREAK) break;
          if (s == ST_RETURN) return ST_RETURN;
          if (s == ST_ERR) return ST_ERR;
        }
        break;
      }
      case Node::S_FORNUM: {
        Value fromV, toV, stepV;
        if (evalExpr(vm, env, st->kids[0], &fromV) != ST_OK) return ST_ERR;
        if (evalExpr(vm, env, st->kids[1], &toV) != ST_OK) return ST_ERR;
        double step = 1;
        if (st->kids.size() > 2 && st->kids[2]) {
          if (evalExpr(vm, env, st->kids[2], &stepV) != ST_OK) return ST_ERR;
          if (!toNum(stepV, &step) || step == 0) step = 1;
        }
        double i = 0, lim = 0;
        if (!toNum(fromV, &i) || !toNum(toV, &lim)) { setErr(vm, "for 边界不是数字"); return ST_ERR; }
        for (; (step > 0 ? i <= lim : i >= lim); i += step) {
          if (!spend(vm)) return ST_ERR;
          auto scope = std::make_shared<Env>();
          scope->parent = std::shared_ptr<Env>(env, [](Env*) {});
          scope->vars[st->str] = makeNum(i);
          const Stat s = execBlock(vm, scope.get(), st->blocks[0], ret);
          if (s == ST_BREAK) break;
          if (s == ST_RETURN) return ST_RETURN;
          if (s == ST_ERR) return ST_ERR;
        }
        break;
      }
      case Node::S_FORIN: {
        std::vector<Value> ctl;
        for (Node* e : st->kids) {
          Value v;
          if (evalExpr(vm, env, e, &v) != ST_OK) return ST_ERR;
          ctl.push_back(v);
        }
        if (ctl.size() < 3) { setErr(vm, "for-in 需要 3 个迭代值"); return ST_ERR; }
        const Value iter = ctl[0], state = ctl[1];
        double idx = 0;
        if (ctl[2].t == Value::NUM) idx = ctl[2].n;
        for (;;) {
          if (!spend(vm)) return ST_ERR;
          std::vector<Value> args{state, makeNum(idx + 1)};
          std::vector<Value> rets;
          if (callValue(vm, env, iter, args, &rets) != ST_OK) return ST_ERR;
          if (rets.empty() || rets[0].t == Value::NIL) break;
          ++idx;
          auto scope = std::make_shared<Env>();
          scope->parent = std::shared_ptr<Env>(env, [](Env*) {});
          if (st->names.size() > 0) scope->vars[st->names[0]] = rets[0];
          if (st->names.size() > 1) scope->vars[st->names[1]] = makeNum(idx);
          const Stat s = execBlock(vm, scope.get(), st->blocks[0], ret);
          if (s == ST_BREAK) break;
          if (s == ST_RETURN) return ST_RETURN;
          if (s == ST_ERR) return ST_ERR;
        }
        break;
      }
      case Node::S_RETURN: {
        if (st->kids.empty()) { *ret = makeNil(); return ST_RETURN; }
        std::vector<Value> vs;
        if (evalExprMulti(vm, env, st->kids[0], &vs) != ST_OK) return ST_ERR;
        *ret = vs.empty() ? makeNil() : vs[0];
        return ST_RETURN;
      }
      case Node::S_BREAK: return ST_BREAK;
      case Node::S_DO: {
        const Stat s = execBlock(vm, env, st->blocks[0], ret);
        if (s != ST_OK) return s;
        break;
      }
      case Node::S_LOCALFUNC: {
        Value fnv;
        if (evalExpr(vm, env, st->kids[0], &fnv) != ST_OK) return ST_ERR;
        env->vars[st->str] = fnv;
        break;
      }
      case Node::S_FUNCDECL: {
        Value fnv;
        if (evalExpr(vm, env, st->kids[0], &fnv) != ST_OK) return ST_ERR;
        // a.b.c = f ；function a:b() 也一样（方法名在 st->str）
        if (st->names.size() == 1 && st->str.empty()) {
          vm->globals->vars[st->names[0]] = fnv;
        } else if (st->names.size() == 1) {
          Value base;
          if (!lookup(env, st->names[0], &base)) base = makeNil();
          if (base.t != Value::TAB || !base.tab) { setErr(vm, "给非表定义方法"); return ST_ERR; }
          tableSet(base.tab, keyOfStr(st->str), fnv);
        } else {
          Value base;
          if (!lookup(env, st->names[0], &base)) base = makeNil();
          for (size_t i = 1; i + 1 < st->names.size(); ++i) {
            base = indexValue(base, makeStr(st->names[i]));
          }
          if (st->names.size() >= 2) {
            if (base.t != Value::TAB || !base.tab) { setErr(vm, "给非表定义函数"); return ST_ERR; }
            tableSet(base.tab, keyOfStr(st->names.back()), fnv);
          }
        }
        break;
      }
      default:
        setErr(vm, "未知语句");
        return ST_ERR;
    }
  }
  return ST_OK;
}

}  // namespace

// ══════════════ 对外 C API ══════════════
extern "C" {

void* ml_new(void) {
  VM* vm = new VM();
  vm->globals = std::make_shared<Env>();
  installGlobals(vm);
  return vm;
}

void ml_free(void* p) {
  VM* vm = static_cast<VM*>(p);
  if (!vm) return;
  for (Node* nd : vm->pool) delete nd;   // AST 节点随 VM 一起释放
  delete vm;
}

int ml_load(void* p, const char* name, const char* src, size_t len) {
  VM* vm = static_cast<VM*>(p);
  if (!vm || !src) return -1;
  vm->err.clear();
  vm->name = name ? name : "?";
  Lexer lx{src, len};
  Parser ps;
  std::vector<Node*> pool;
  for (;;) {
    Tok t = lx.next();
    if (!lx.err.empty()) { vm->err = lx.err; return -2; }
    ps.toks.push_back(t);
    if (t.t == Tok::END) break;
  }
  std::vector<Node*> ast;
  while (ps.cur().t != Tok::END) {
    Node* st = nullptr;
    if (!ps.statement(&st)) { vm->err = ps.err; return -3; }
    ast.push_back(st);
  }
  vm->ast = ast;
  for (Node* nd : ps.pool) vm->pool.push_back(nd);   // 交给 VM 统一释放
  Value ret;
  const Stat s = execBlock(vm, vm->globals.get(), vm->ast, &ret);
  if (s == ST_ERR) return -4;
  return 0;
}

int ml_call(void* p, const char* fn, const char* const* argv, int argc, char* out, int outCap) {
  VM* vm = static_cast<VM*>(p);
  if (!vm) return -1;
  if (out && outCap > 0) out[0] = 0;
  vm->err.clear();
  vm->budget = 20000000;
  Value f;
  if (!lookup(vm->globals.get(), fn ? fn : "", &f)) { vm->err = "脚本里没有这个函数"; return -2; }
  std::vector<Value> args;
  for (int i = 0; i < argc; ++i) args.push_back(argv[i] ? makeStr(argv[i]) : makeNil());
  std::vector<Value> rets;
  if (callValue(vm, vm->globals.get(), f, args, &rets) != ST_OK) return -3;
  if (out && outCap > 0 && !rets.empty()) {
    const std::string s = toStr(rets[0]);
    std::snprintf(out, (size_t)outCap, "%s", s.c_str());
  }
  return 0;
}

const char* ml_error(void* p) {
  VM* vm = static_cast<VM*>(p);
  return vm ? vm->err.c_str() : "vm 为空";
}

int ml_selftest(void) {
  static const char* kScript =
      "local t = {1, 2, 3}\n"
      "local n = 0\n"
      "for i = 1, 3 do n = n + i end\n"
      "local s = ''\n"
      "for _, v in ipairs(t) do s = s .. tostring(v) end\n"
      "local function add(a, b) return a + b end\n"
      "function probe()\n"
      "  if n == 6 and s == '123' and add(2, 3) == 5 then return 'ok' end\n"
      "  return 'bad'\n"
      "end\n";
  void* vm = ml_new();
  if (!vm) return -1;
  const int rc = ml_load(vm, "selftest", kScript, std::strlen(kScript));
  if (rc != 0) { ml_free(vm); return -2; }
  char buf[16] = {0};
  const int cr = ml_call(vm, "probe", nullptr, 0, buf, sizeof(buf));
  const bool ok = cr == 0 && std::strcmp(buf, "ok") == 0;
  ml_free(vm);
  return ok ? 0 : -3;
}

}  // extern "C"
