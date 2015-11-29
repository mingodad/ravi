/*
** $Id: lapi.c,v 2.257 2015/11/02 18:48:07 roberto Exp $
** Lua API
** See Copyright Notice in lua.h
*/

/*
** Portions Copyright (C) 2015 Dibyendu Majumdar
*/


#define lapi_c
#define LUA_CORE

#include "lprefix.h"


#include <stdarg.h>
#include <string.h>

#include "lua.h"

#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lundump.h"
#include "lvm.h"



const char lua_ident[] =
  "$LuaVersion: " LUA_COPYRIGHT " $"
  "$LuaAuthors: " LUA_AUTHORS " $";


/* value at a non-valid index */
#define NONVALIDVALUE		cast(TValue *, luaO_nilobject)

/* corresponding test */
#define isvalid(o)	((o) != luaO_nilobject)

/* test for pseudo index */
#define ispseudo(i)		((i) <= LUA_REGISTRYINDEX)

/* test for upvalue */
#define isupvalue(i)		((i) < LUA_REGISTRYINDEX)

/* test for valid but not pseudo index */
#define isstackindex(i, o)	(isvalid(o) && !ispseudo(i))

#define api_checkvalidindex(l,o)  api_check(l, isvalid(o), "invalid index")

#define api_checkstackindex(l, i, o)  \
	api_check(l, isstackindex(i, o), "index not in the stack")


static TValue *index2addr (lua_State *L, int idx) {
  CallInfo *ci = L->ci;
  if (idx > 0) {
    TValue *o = ci->func + idx;
    api_check(L, idx <= ci->top - (ci->func + 1), "unacceptable index");
    if (o >= L->top) return NONVALIDVALUE;
    else return o;
  }
  else if (!ispseudo(idx)) {  /* negative index */
    api_check(L, idx != 0 && -idx <= L->top - (ci->func + 1), "invalid index");
    return L->top + idx;
  }
  else if (idx == LUA_REGISTRYINDEX)
    return &G(L)->l_registry;
  else {  /* upvalues */
    idx = LUA_REGISTRYINDEX - idx;
    api_check(L, idx <= MAXUPVAL + 1, "upvalue index too large");
    if (ttislcf(ci->func))  /* light C function? */
      return NONVALIDVALUE;  /* it has no upvalues */
    else {
      CClosure *func = clCvalue(ci->func);
      return (idx <= func->nupvalues) ? &func->upvalue[idx-1] : NONVALIDVALUE;
    }
  }
}


/*
** to be called by 'lua_checkstack' in protected mode, to grow stack
** capturing memory errors
*/
static void growstack (lua_State *L, void *ud) {
  int size = *(int *)ud;
  luaD_growstack(L, size);
}


LUA_API int lua_checkstack (lua_State *L, int n) {
  int res;
  CallInfo *ci = L->ci;
  lua_lock(L);
  api_check(L, n >= 0, "negative 'n'");
  if (L->stack_last - L->top > n)  /* stack large enough? */
    res = 1;  /* yes; check is OK */
  else {  /* no; need to grow stack */
    int inuse = cast_int(L->top - L->stack) + EXTRA_STACK;
    if (inuse > LUAI_MAXSTACK - n)  /* can grow without overflow? */
      res = 0;  /* no */
    else  /* try to grow stack */
      res = (luaD_rawrunprotected(L, &growstack, &n) == LUA_OK);
  }
  if (res && ci->top < L->top + n)
    ci->top = L->top + n;  /* adjust frame top */
  lua_unlock(L);
  return res;
}


LUA_API void lua_xmove (lua_State *from, lua_State *to, int n) {
  int i;
  if (from == to) return;
  lua_lock(to);
  api_checknelems(from, n);
  api_check(from, G(from) == G(to), "moving among independent states");
  api_check(from, to->ci->top - to->top >= n, "stack overflow");
  from->top -= n;
  for (i = 0; i < n; i++) {
    setobj2s(to, to->top, from->top + i);
    to->top++;  /* stack already checked by previous 'api_check' */
  }
  lua_unlock(to);
}


LUA_API lua_CFunction lua_atpanic (lua_State *L, lua_CFunction panicf) {
  lua_CFunction old;
  lua_lock(L);
  old = G(L)->panic;
  G(L)->panic = panicf;
  lua_unlock(L);
  return old;
}


LUA_API const lua_Number *lua_version (lua_State *L) {
  static const lua_Number version = LUA_VERSION_NUM;
  if (L == NULL) return &version;
  else return G(L)->version;
}



/*
** basic stack manipulation
*/


/*
** convert an acceptable stack index into an absolute index
*/
LUA_API int lua_absindex (lua_State *L, int idx) {
  return (idx > 0 || ispseudo(idx))
         ? idx
         : cast_int(L->top - L->ci->func) + idx;
}


LUA_API int lua_gettop (lua_State *L) {
  return cast_int(L->top - (L->ci->func + 1));
}


LUA_API void lua_settop (lua_State *L, int idx) {
  StkId func = L->ci->func;
  lua_lock(L);
  if (idx >= 0) {
    api_check(L, idx <= L->stack_last - (func + 1), "new top too large");
    while (L->top < (func + 1) + idx)
      setnilvalue(L->top++);
    L->top = (func + 1) + idx;
  }
  else {
    api_check(L, -(idx+1) <= (L->top - (func + 1)), "invalid new top");
    L->top += idx+1;  /* 'subtract' index (index is negative) */
  }
  lua_unlock(L);
}


/*
** Reverse the stack segment from 'from' to 'to'
** (auxiliary to 'lua_rotate')
*/
static void reverse (lua_State *L, StkId from, StkId to) {
  for (; from < to; from++, to--) {
    TValue temp;
    setobj(L, &temp, from);
    setobjs2s(L, from, to);
    setobj2s(L, to, &temp);
  }
}


/*
** Let x = AB, where A is a prefix of length 'n'. Then,
** rotate x n == BA. But BA == (A^r . B^r)^r.
*/
LUA_API void lua_rotate (lua_State *L, int idx, int n) {
  StkId p, t, m;
  lua_lock(L);
  t = L->top - 1;  /* end of stack segment being rotated */
  p = index2addr(L, idx);  /* start of segment */
  api_checkstackindex(L, idx, p);
  api_check(L, (n >= 0 ? n : -n) <= (t - p + 1), "invalid 'n'");
  m = (n >= 0 ? t - n : p - n - 1);  /* end of prefix */
  reverse(L, p, m);  /* reverse the prefix with length 'n' */
  reverse(L, m + 1, t);  /* reverse the suffix */
  reverse(L, p, t);  /* reverse the entire segment */
  lua_unlock(L);
}


LUA_API void lua_copy (lua_State *L, int fromidx, int toidx) {
  TValue *fr, *to;
  lua_lock(L);
  fr = index2addr(L, fromidx);
  to = index2addr(L, toidx);
  api_checkvalidindex(L, to);
  setobj(L, to, fr);
  if (isupvalue(toidx))  /* function upvalue? */
    luaC_barrier(L, clCvalue(L->ci->func), fr);
  /* LUA_REGISTRYINDEX does not need gc barrier
     (collector revisits it before finishing collection) */
  lua_unlock(L);
}


LUA_API void lua_pushvalue (lua_State *L, int idx) {
  lua_lock(L);
  setobj2s(L, L->top, index2addr(L, idx));
  api_incr_top(L);
  lua_unlock(L);
}



/*
** access functions (stack -> C)
*/


LUA_API int lua_type (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  return (isvalid(o) ? ttnov(o) : LUA_TNONE);
}


LUA_API const char *lua_typename (lua_State *L, int t) {
  UNUSED(L);
  api_check(L, LUA_TNONE <= t && t < LUA_NUMTAGS, "invalid tag");
  return ttypename(t);
}

/* 
Ravi extension
*/
LUA_API const char *ravi_typename(lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  switch (rttype(o)) {
    case LUA_TNIL: return "nil";
    case LUA_TBOOLEAN: return "boolean";
    case LUA_TNUMFLT: return "number";
    case LUA_TNUMINT: return "integer";
    case ctb(LUA_TLNGSTR): return "string";
    case ctb(LUA_TSHRSTR): return "string";
    case ctb(LUA_TUSERDATA): return "userdata";
    case LUA_TLIGHTUSERDATA: return "lightuserdata";
    case LUA_TLCF: return "lightCfunction";
    case ctb(LUA_TCCL): return "Cclosure";
    case ctb(LUA_TLCL): return "closure";
    case ctb(LUA_TTHREAD): return "thread";
    case ctb(LUA_TTABLE): {
      Table *h = hvalue(o);
      switch (h->ravi_array.array_type) {
        case RAVI_TARRAYFLT: return "number[]";
        case RAVI_TARRAYINT: return "integer[]";
        default: return "table";
      }
    }
    default: return "unknown";
  }
}

LUA_API int lua_iscfunction (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  return (ttislcf(o) || (ttisCclosure(o)));
}


LUA_API int lua_isinteger (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  return ttisinteger(o);
}


LUA_API int lua_isnumber (lua_State *L, int idx) {
  lua_Number n;
  const TValue *o = index2addr(L, idx);
  return tonumber(o, &n);
}


LUA_API int lua_isstring (lua_State *L, int idx) {
  const TValue *o = index2addr(L, idx);
  return (ttisstring(o) || cvt2str(o));
}


LUA_API int lua_isuserdata (lua_State *L, int idx) {
  const TValue *o = index2addr(L, idx);
  return (ttisfulluserdata(o) || ttislightuserdata(o));
}


LUA_API int lua_rawequal (lua_State *L, int index1, int index2) {
  StkId o1 = index2addr(L, index1);
  StkId o2 = index2addr(L, index2);
  return (isvalid(o1) && isvalid(o2)) ? luaV_rawequalobj(o1, o2) : 0;
}


LUA_API void lua_arith (lua_State *L, int op) {
  lua_lock(L);
  if (op != LUA_OPUNM && op != LUA_OPBNOT)
    api_checknelems(L, 2);  /* all other operations expect two operands */
  else {  /* for unary operations, add fake 2nd operand */
    api_checknelems(L, 1);
    setobjs2s(L, L->top, L->top - 1);
    api_incr_top(L);
  }
  /* first operand at top - 2, second at top - 1; result go to top - 2 */
  luaO_arith(L, op, L->top - 2, L->top - 1, L->top - 2);
  L->top--;  /* remove second operand */
  lua_unlock(L);
}


LUA_API int lua_compare (lua_State *L, int index1, int index2, int op) {
  StkId o1, o2;
  int i = 0;
  lua_lock(L);  /* may call tag method */
  o1 = index2addr(L, index1);
  o2 = index2addr(L, index2);
  if (isvalid(o1) && isvalid(o2)) {
    switch (op) {
      case LUA_OPEQ: i = luaV_equalobj(L, o1, o2); break;
      case LUA_OPLT: i = luaV_lessthan(L, o1, o2); break;
      case LUA_OPLE: i = luaV_lessequal(L, o1, o2); break;
      default: api_check(L, 0, "invalid option");
    }
  }
  lua_unlock(L);
  return i;
}


LUA_API size_t lua_stringtonumber (lua_State *L, const char *s) {
  size_t sz = luaO_str2num(s, L->top);
  if (sz != 0)
    api_incr_top(L);
  return sz;
}


LUA_API lua_Number lua_tonumberx (lua_State *L, int idx, int *pisnum) {
  lua_Number n;
  const TValue *o = index2addr(L, idx);
  int isnum = tonumber(o, &n);
  if (!isnum)
    n = 0;  /* call to 'tonumber' may change 'n' even if it fails */
  if (pisnum) *pisnum = isnum;
  return n;
}


LUA_API lua_Integer lua_tointegerx (lua_State *L, int idx, int *pisnum) {
  lua_Integer res;
  const TValue *o = index2addr(L, idx);
  int isnum = tointeger(o, &res);
  if (!isnum)
    res = 0;  /* call to 'tointeger' may change 'n' even if it fails */
  if (pisnum) *pisnum = isnum;
  return res;
}


LUA_API int lua_toboolean (lua_State *L, int idx) {
  const TValue *o = index2addr(L, idx);
  return !l_isfalse(o);
}


LUA_API const char *lua_tolstring (lua_State *L, int idx, size_t *len) {
  StkId o = index2addr(L, idx);
  if (!ttisstring(o)) {
    if (!cvt2str(o)) {  /* not convertible? */
      if (len != NULL) *len = 0;
      return NULL;
    }
    lua_lock(L);  /* 'luaO_tostring' may create a new string */
    luaC_checkGC(L);
    o = index2addr(L, idx);  /* previous call may reallocate the stack */
    luaO_tostring(L, o);
    lua_unlock(L);
  }
  if (len != NULL)
    *len = vslen(o);
  return svalue(o);
}


LUA_API size_t lua_rawlen (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  switch (ttype(o)) {
    case LUA_TSHRSTR: return tsvalue(o)->shrlen;
    case LUA_TLNGSTR: return tsvalue(o)->u.lnglen;
    case LUA_TUSERDATA: return uvalue(o)->len;
    case LUA_TTABLE: {
      Table *h = hvalue(o);
      switch (h->ravi_array.array_type) {
        case RAVI_TTABLE: return luaH_getn(h);
        default: return raviH_getn(h);
      }
    }
    default: return 0;
  }
}


LUA_API lua_CFunction lua_tocfunction (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  if (ttislcf(o)) return fvalue(o);
  else if (ttisCclosure(o))
    return clCvalue(o)->f;
  else return NULL;  /* not a C function */
}


LUA_API void *lua_touserdata (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  switch (ttnov(o)) {
    case LUA_TUSERDATA: return getudatamem(uvalue(o));
    case LUA_TLIGHTUSERDATA: return pvalue(o);
    default: return NULL;
  }
}


LUA_API lua_State *lua_tothread (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  return (!ttisthread(o)) ? NULL : thvalue(o);
}


LUA_API const void *lua_topointer (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  switch (ttype(o)) {
    case LUA_TTABLE: return hvalue(o);
    case LUA_TLCL: return clLvalue(o);
    case LUA_TCCL: return clCvalue(o);
    case LUA_TLCF: return cast(void *, cast(size_t, fvalue(o)));
    case LUA_TTHREAD: return thvalue(o);
    case LUA_TUSERDATA: return getudatamem(uvalue(o));
    case LUA_TLIGHTUSERDATA: return pvalue(o);
    default: return NULL;
  }
}



/*
** push functions (C -> stack)
*/


LUA_API void lua_pushnil (lua_State *L) {
  lua_lock(L);
  setnilvalue(L->top);
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_pushnumber (lua_State *L, lua_Number n) {
  lua_lock(L);
  setfltvalue(L->top, n);
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_pushinteger (lua_State *L, lua_Integer n) {
  lua_lock(L);
  setivalue(L->top, n);
  api_incr_top(L);
  lua_unlock(L);
}


/*
** Pushes on the stack a string with given length. Avoid using 's' when
** 'len' == 0 (as 's' can be NULL in that case), due to later use of
** 'memcmp' and 'memcpy'.
*/
LUA_API const char *lua_pushlstring (lua_State *L, const char *s, size_t len) {
  TString *ts;
  lua_lock(L);
  luaC_checkGC(L);
  ts = (len == 0) ? luaS_new(L, "") : luaS_newlstr(L, s, len);
  setsvalue2s(L, L->top, ts);
  api_incr_top(L);
  lua_unlock(L);
  return getstr(ts);
}


LUA_API const char *lua_pushstring (lua_State *L, const char *s) {
  lua_lock(L);
  if (s == NULL)
    setnilvalue(L->top);
  else {
    TString *ts;
    luaC_checkGC(L);
    ts = luaS_new(L, s);
    setsvalue2s(L, L->top, ts);
    s = getstr(ts);  /* internal copy's address */
  }
  api_incr_top(L);
  lua_unlock(L);
  return s;
}


LUA_API const char *lua_pushvfstring (lua_State *L, const char *fmt,
                                      va_list argp) {
  const char *ret;
  lua_lock(L);
  luaC_checkGC(L);
  ret = luaO_pushvfstring(L, fmt, argp);
  lua_unlock(L);
  return ret;
}


LUA_API const char *lua_pushfstring (lua_State *L, const char *fmt, ...) {
  const char *ret;
  va_list argp;
  lua_lock(L);
  luaC_checkGC(L);
  va_start(argp, fmt);
  ret = luaO_pushvfstring(L, fmt, argp);
  va_end(argp);
  lua_unlock(L);
  return ret;
}


LUA_API void lua_pushcclosure (lua_State *L, lua_CFunction fn, int n) {
  lua_lock(L);
  if (n == 0) {
    setfvalue(L->top, fn);
  }
  else {
    CClosure *cl;
    api_checknelems(L, n);
    if (n > MAXUPVAL)
      luaG_runerror(L, "upvalue index too large");
    api_check(L, n <= MAXUPVAL, "upvalue index too large");
    luaC_checkGC(L);
    cl = luaF_newCclosure(L, n);
    cl->f = fn;
    L->top -= n;
    while (n--) {
      setobj2n(L, &cl->upvalue[n], L->top + n);
      /* does not need barrier because closure is white */
    }
    setclCvalue(L, L->top, cl);
  }
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_pushboolean (lua_State *L, int b) {
  lua_lock(L);
  setbvalue(L->top, (b != 0));  /* ensure that true is 1 */
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_pushlightuserdata (lua_State *L, void *p) {
  lua_lock(L);
  setpvalue(L->top, p);
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API int lua_pushthread (lua_State *L) {
  lua_lock(L);
  setthvalue(L, L->top, L);
  api_incr_top(L);
  lua_unlock(L);
  return (G(L)->mainthread == L);
}



/*
** get functions (Lua -> stack)
*/


static int auxgetstr (lua_State *L, const TValue *t, const char *k) {
  const TValue *aux;
  TString *str = luaS_new(L, k);
  if (luaV_fastget(L, t, str, aux, luaH_getstr)) {
    setobj2s(L, L->top, aux);
    api_incr_top(L);
  }
  else {
    setsvalue2s(L, L->top, str);
    api_incr_top(L);
    luaV_finishget(L, t, L->top - 1, L->top - 1, aux);
  }
  lua_unlock(L);
  return ttnov(L->top - 1);
}


LUA_API int lua_getglobal (lua_State *L, const char *name) {
  Table *reg = hvalue(&G(L)->l_registry);
  lua_lock(L);
  return auxgetstr(L, luaH_getint(reg, LUA_RIDX_GLOBALS), name);
}


LUA_API int lua_gettable (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  t = index2addr(L, idx);
  luaV_gettable(L, t, L->top - 1, L->top - 1);
  lua_unlock(L);
  return ttnov(L->top - 1);
}


LUA_API int lua_getfield (lua_State *L, int idx, const char *k) {
  lua_lock(L);
  return auxgetstr(L, index2addr(L, idx), k);
}

LUA_API int lua_geti(lua_State *L, int idx, lua_Integer n) {
  StkId t;
  const TValue *aux;
  lua_lock(L);
  t = index2addr(L, idx);
  if (ttistable(t)) {
    Table *h = hvalue(t);
    switch (h->ravi_array.array_type) {
      case RAVI_TARRAYINT: {
        if (n <= raviH_getn(h)) { raviH_get_int_inline(L, h, n, L->top); }
        else {
          setnilvalue(L->top);
        }
        api_incr_top(L);
      } break;
      case RAVI_TARRAYFLT: {
        if (n <= raviH_getn(h)) { raviH_get_float_inline(L, h, n, L->top); }
        else {
          setnilvalue(L->top);
        }
        api_incr_top(L);
      } break;
      default: {
        if (luaV_fastget(L, t, n, aux, luaH_getint)) {
          setobj2s(L, L->top, aux);
          api_incr_top(L);
        }
        else {
          setivalue(L->top, n);
          api_incr_top(L);
          luaV_finishget(L, t, L->top - 1, L->top - 1, aux);
        }
      }
    }
  }
  else {
    if (luaV_fastget(L, t, n, aux, luaH_getint)) {
      setobj2s(L, L->top, aux);
      api_incr_top(L);
    }
    else {
      setivalue(L->top, n);
      api_incr_top(L);
      luaV_finishget(L, t, L->top - 1, L->top - 1, aux);
    }
  }
  lua_unlock(L);
  return ttnov(L->top - 1);
}

LUA_API int lua_rawget(lua_State *L, int idx) {
  StkId t;
  Table *h;
  lua_lock(L);
  t = index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  h = hvalue(t);
  switch (h->ravi_array.array_type) {
    case RAVI_TARRAYINT: {
      TValue *key = L->top - 1;
      api_check(L, ttisinteger(key), "key must be integer");
      if (ttisinteger(key)) {
        lua_Integer n = ivalue(key);
        if (n <= raviH_getn(h)) { raviH_get_int_inline(L, h, n, key); }
        else {
          setnilvalue(key);
        }
      }
      else {
        setnilvalue(key);
      }
    } break;
    case RAVI_TARRAYFLT: {
      TValue *key = L->top - 1;
      api_check(L, ttisinteger(key), "key must be integer");
      if (ttisinteger(key)) {
        lua_Integer n = ivalue(key);
        if (n <= raviH_getn(h)) { raviH_get_float_inline(L, h, n, key); }
        else {
          setnilvalue(key);
        }
      }
      else {
        setnilvalue(key);
      }
    } break;
    default: setobj2s(L, L->top - 1, luaH_get(hvalue(t), L->top - 1)); break;
  }
  lua_unlock(L);
  return ttnov(L->top - 1);
}

LUA_API int lua_rawgeti(lua_State *L, int idx, lua_Integer n) {
  StkId t;
  Table *h;
  lua_lock(L);
  t = index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  h = hvalue(t);
  switch (h->ravi_array.array_type) {
    case RAVI_TARRAYINT: {
      if (n <= raviH_getn(h)) { raviH_get_int_inline(L, h, n, L->top); }
      else {
        setnilvalue(L->top);
      }
    } break;
    case RAVI_TARRAYFLT: {
      if (n <= raviH_getn(h)) { raviH_get_float_inline(L, h, n, L->top); }
      else {
        setnilvalue(L->top);
      }
    } break;
    default: setobj2s(L, L->top, luaH_getint(hvalue(t), n)); break;
  }
  api_incr_top(L);
  lua_unlock(L);
  return ttnov(L->top - 1);
}

LUA_API int lua_rawgetp (lua_State *L, int idx, const void *p) {
  StkId t;
  TValue k;
  Table *h;
  lua_lock(L);
  t = index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  h = hvalue(t);
  api_check(L, h->ravi_array.array_type == RAVI_TTABLE, "Lua table expected");
  setpvalue(&k, cast(void *, p));
  setobj2s(L, L->top, luaH_get(h, &k));
  api_incr_top(L);
  lua_unlock(L);
  return ttnov(L->top - 1);
}


LUA_API void lua_createtable (lua_State *L, int narray, int nrec) {
  Table *t;
  lua_lock(L);
  luaC_checkGC(L);
  t = luaH_new(L);
  sethvalue(L, L->top, t);
  api_incr_top(L);
  if (narray > 0 || nrec > 0)
    luaH_resize(L, t, narray, nrec);
  lua_unlock(L);
}

/* Create an integer array (specialization of Lua table)
 * of given size and initialize array with supplied initial value
 */
LUA_API void ravi_create_integer_array(lua_State *L, int narray,
                                     lua_Integer initial_value) {
  Table *t;
  lua_lock(L);
  luaC_checkGC(L);
  t = raviH_new_integer_array(L, (unsigned int)narray, initial_value);
  sethvalue(L, L->top, t);
  api_incr_top(L);
  lua_unlock(L);
}

/* Create an number array (specialization of Lua table)
 * of given size and initialize array with supplied initial value
 */
LUA_API void ravi_create_number_array(lua_State *L, int narray,
                                    lua_Number initial_value) {
  Table *t;
  lua_lock(L);
  luaC_checkGC(L);
  t = raviH_new_number_array(L, (unsigned int)narray, initial_value);
  sethvalue(L, L->top, t);
  api_incr_top(L);
  lua_unlock(L);
}

/* Tests if the argument is a number array
 */
LUA_API int ravi_is_number_array(lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  return (ttistable(o) && hvalue(o)->ravi_array.array_type == RAVI_TARRAYFLT
              ? 1
              : 0);
}

/* Tests if the argument is a integer array
*/
LUA_API int ravi_is_integer_array(lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  return (ttistable(o) && hvalue(o)->ravi_array.array_type == RAVI_TARRAYINT
              ? 1
              : 0);
}

/* Get the raw data associated with the number array at idx.
 * Note that Ravi arrays have an extra element at offset 0 - this
 * function returns a pointer to &data[0] - bear in mind that
 */
LUA_API lua_Number* ravi_get_number_array_rawdata(lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  lua_assert(ttistable(o) && hvalue(o)->ravi_array.array_type == RAVI_TARRAYFLT);
  double *startp, *endp;
  raviH_get_number_array_rawdata(L, hvalue(o), &startp, &endp);
  return startp;
}

/* Create a slice of an existing array
 * The original table containing the array is inserted into the
 * the slice as a value against special key so that
 * the parent table is not garbage collected while this array contains a
 * reference to it
 * The array slice starts at start but start-1 is also accessible because of the
 * implementation having array values starting at 0.
 * A slice must not attempt to release the data array as this is not owned by
 * it,
 * and in fact may point to garbage from a memory allocater's point of view.
 */
LUA_API void ravi_create_slice(lua_State *L, int idx, unsigned int start,
                               unsigned int len) {
  TValue *parent;
  Table *slice;
  lua_lock(L);
  const char *errmsg = NULL;
  /* The do-while loop here is just for error handling */
  parent = index2addr(L, idx);
  if (!ttistable(parent)) {
    errmsg = "integer[] or number[] expected";
    goto done;
  }
  Table *orig = hvalue(parent);
  if (orig->ravi_array.array_type == RAVI_TTABLE) {
    errmsg = "cannot create a slice of a table, integer[] or number[] expected";
    goto done;
  }
  if (start < 1 || start + len > orig->ravi_array.len) {
    errmsg = "cannot create a slice of given bounds";
    goto done;
  }
  luaC_checkGC(L);
  slice = raviH_new_slice(L, parent, start, len);
  sethvalue(L, L->top, slice);
  api_incr_top(L);
done:
  lua_unlock(L);
  if (errmsg)
    luaG_runerror(L, errmsg);
}

LUA_API int lua_getmetatable (lua_State *L, int objindex) {
  const TValue *obj;
  Table *mt;
  int res = 0;
  lua_lock(L);
  obj = index2addr(L, objindex);
  switch (ttnov(obj)) {
    case LUA_TTABLE:
      mt = hvalue(obj)->metatable;
      break;
    case LUA_TUSERDATA:
      mt = uvalue(obj)->metatable;
      break;
    default:
      mt = G(L)->mt[ttnov(obj)];
      break;
  }
  if (mt != NULL) {
    sethvalue(L, L->top, mt);
    api_incr_top(L);
    res = 1;
  }
  lua_unlock(L);
  return res;
}


LUA_API int lua_getuservalue (lua_State *L, int idx) {
  StkId o;
  lua_lock(L);
  o = index2addr(L, idx);
  api_check(L, ttisfulluserdata(o), "full userdata expected");
  getuservalue(L, uvalue(o), L->top);
  api_incr_top(L);
  lua_unlock(L);
  return ttnov(L->top - 1);
}


/*
** set functions (stack -> Lua)
*/

/*
** t[k] = value at the top of the stack (where 'k' is a string)
*/
static void auxsetstr (lua_State *L, const TValue *t, const char *k) {
  const TValue *aux;
  TString *str = luaS_new(L, k);
  api_checknelems(L, 1);
  if (luaV_fastset(L, t, str, aux, luaH_getstr, L->top - 1))
    L->top--;  /* pop value */
  else {
    setsvalue2s(L, L->top, str);  /* push 'str' (to make it a TValue) */
    api_incr_top(L);
    luaV_finishset(L, t, L->top - 1, L->top - 2, aux);
    L->top -= 2;  /* pop value and key */
  }
  lua_unlock(L);  /* lock done by caller */
}


LUA_API void lua_setglobal (lua_State *L, const char *name) {
  Table *reg = hvalue(&G(L)->l_registry);
  lua_lock(L);  /* unlock done in 'auxsetstr' */
  auxsetstr(L, luaH_getint(reg, LUA_RIDX_GLOBALS), name);
}


LUA_API void lua_settable (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  api_checknelems(L, 2);
  t = index2addr(L, idx);
  luaV_settable(L, t, L->top - 2, L->top - 1);
  L->top -= 2;  /* pop index and value */
  lua_unlock(L);
}


LUA_API void lua_setfield (lua_State *L, int idx, const char *k) {
  lua_lock(L);  /* unlock done in 'auxsetstr' */
  auxsetstr(L, index2addr(L, idx), k);
}

LUA_API void lua_seti(lua_State *L, int idx, lua_Integer n) {
  StkId t;
  const TValue *aux;
  lua_lock(L);
  api_checknelems(L, 1);
  t = index2addr(L, idx);
  if (ttistable(t)) {
    Table *h = hvalue(t);
    switch (h->ravi_array.array_type) {
      case RAVI_TARRAYINT: {
        TValue *val = L->top - 1;
        if (ttisinteger(val)) { raviH_set_int_inline(L, h, n, ivalue(val)); }
        else {
          lua_Integer i = 0;
          if (luaV_tointeger_(val, &i)) { raviH_set_int_inline(L, h, n, i); }
          else
            luaG_runerror(L, "value cannot be converted to integer");
        }
        L->top--;
      } break;
      case RAVI_TARRAYFLT: {
        TValue *val = L->top - 1;
        if (ttisfloat(val)) { raviH_set_float_inline(L, h, n, fltvalue(val)); }
        else if (ttisinteger(val)) {
          raviH_set_float_inline(L, h, n, (lua_Number)(ivalue(val)));
        }
        else {
          lua_Number d = 0.0;
          if (luaV_tonumber_(val, &d)) { raviH_set_float_inline(L, h, n, d); }
          else
            luaG_runerror(L, "value cannot be converted to number");
        }
        L->top--;
      } break;
      default: {
        if (luaV_fastset(L, t, n, aux, luaH_getint, L->top - 1))
          L->top--; /* pop value */
        else {
          setivalue(L->top, n);
          api_incr_top(L);
          luaV_finishset(L, t, L->top - 1, L->top - 2, aux);
          L->top -= 2; /* pop value and key */
        }
      }
    }
  }
  else {
    if (luaV_fastset(L, t, n, aux, luaH_getint, L->top - 1))
      L->top--; /* pop value */
    else {
      setivalue(L->top, n);
      api_incr_top(L);
      luaV_finishset(L, t, L->top - 1, L->top - 2, aux);
      L->top -= 2; /* pop value and key */
    }
  }
  lua_unlock(L);
}

LUA_API void lua_rawset(lua_State *L, int idx) {
  StkId o;
  Table *t;
  lua_lock(L);
  api_checknelems(L, 2);
  o = index2addr(L, idx);
  api_check(L, ttistable(o), "table expected");
  t = hvalue(o);
  switch (t->ravi_array.array_type) {
    case RAVI_TARRAYINT: {
      TValue *key = L->top - 2;
      TValue *val = L->top - 1;
      if (!ttisinteger(key)) luaG_typeerror(L, key, "index");
      if (ttisinteger(val)) {
        raviH_set_int_inline(L, t, ivalue(key), ivalue(val));
      }
      else {
        lua_Integer i = 0;
        if (luaV_tointeger_(val, &i)) {
          raviH_set_int_inline(L, t, ivalue(key), i);
        }
        else
          luaG_runerror(L, "value cannot be converted to integer");
      }
    } break;
    case RAVI_TARRAYFLT: {
      TValue *key = L->top - 2;
      TValue *val = L->top - 1;
      if (!ttisinteger(key)) luaG_typeerror(L, key, "index");
      if (ttisfloat(val)) {
        raviH_set_float_inline(L, t, ivalue(key), fltvalue(val));
      }
      else if (ttisinteger(val)) {
        raviH_set_float_inline(L, t, ivalue(key), (lua_Number)(ivalue(val)));
      }
      else {
        lua_Number d = 0.0;
        if (luaV_tonumber_(val, &d)) {
          raviH_set_float_inline(L, t, ivalue(key), d);
        }
        else
          luaG_runerror(L, "value cannot be converted to number");
      }
    } break;
    default: setobj2t(L, luaH_set(L, t, L->top - 2), L->top - 1); break;
  }
  invalidateTMcache(t);
  luaC_barrierback(L, t, L->top - 1);
  L->top -= 2;
  lua_unlock(L);
}

LUA_API void lua_rawseti(lua_State *L, int idx, lua_Integer n) {
  StkId o;
  Table *t;
  lua_lock(L);
  api_checknelems(L, 1);
  o = index2addr(L, idx);
  api_check(L, ttistable(o), "table expected");
  t = hvalue(o);
  switch (t->ravi_array.array_type) {
    case RAVI_TARRAYINT: {
      TValue *val = L->top - 1;
      if (ttisinteger(val)) { raviH_set_int_inline(L, t, n, ivalue(val)); }
      else {
        lua_Integer i = 0;
        if (luaV_tointeger_(val, &i)) { raviH_set_int_inline(L, t, n, i); }
        else
          luaG_runerror(L, "value cannot be converted to integer");
      }
    } break;
    case RAVI_TARRAYFLT: {
      TValue *val = L->top - 1;
      if (ttisfloat(val)) { raviH_set_float_inline(L, t, n, fltvalue(val)); }
      else if (ttisinteger(val)) {
        raviH_set_float_inline(L, t, n, (lua_Number)(ivalue(val)));
      }
      else {
        lua_Number d = 0.0;
        if (luaV_tonumber_(val, &d)) { raviH_set_float_inline(L, t, n, d); }
        else
          luaG_runerror(L, "value cannot be converted to number");
      }
    } break;
    default: luaH_setint(L, t, n, L->top - 1); break;
  }
  luaC_barrierback(L, t, L->top - 1);
  L->top--;
  lua_unlock(L);
}

LUA_API void lua_rawsetp (lua_State *L, int idx, const void *p) {
  StkId o;
  Table *t;
  TValue k;
  lua_lock(L);
  api_checknelems(L, 1);
  o = index2addr(L, idx);
  api_check(L, ttistable(o), "table expected");
  t = hvalue(o);
  api_check(L, t->ravi_array.array_type == RAVI_TTABLE, "Lua table expected");
  setpvalue(&k, cast(void *, p));
  setobj2t(L, luaH_set(L, t, &k), L->top - 1);
  luaC_barrierback(L, t, L->top - 1);
  L->top--;
  lua_unlock(L);
}


LUA_API int lua_setmetatable (lua_State *L, int objindex) {
  TValue *obj;
  Table *mt;
  lua_lock(L);
  api_checknelems(L, 1);
  obj = index2addr(L, objindex);
  if (ttisnil(L->top - 1))
    mt = NULL;
  else {
    api_check(L, ttistable(L->top - 1), "table expected");
    mt = hvalue(L->top - 1);
  }
  switch (ttnov(obj)) {
    case LUA_TTABLE: {
      hvalue(obj)->metatable = mt;
      if (mt) {
        luaC_objbarrier(L, gcvalue(obj), mt);
        luaC_checkfinalizer(L, gcvalue(obj), mt);
      }
      break;
    }
    case LUA_TUSERDATA: {
      uvalue(obj)->metatable = mt;
      if (mt) {
        luaC_objbarrier(L, uvalue(obj), mt);
        luaC_checkfinalizer(L, gcvalue(obj), mt);
      }
      break;
    }
    default: {
      G(L)->mt[ttnov(obj)] = mt;
      break;
    }
  }
  L->top--;
  lua_unlock(L);
  return 1;
}


LUA_API void lua_setuservalue (lua_State *L, int idx) {
  StkId o;
  lua_lock(L);
  api_checknelems(L, 1);
  o = index2addr(L, idx);
  api_check(L, ttisfulluserdata(o), "full userdata expected");
  setuservalue(L, uvalue(o), L->top - 1);
  luaC_barrier(L, gcvalue(o), L->top - 1);
  L->top--;
  lua_unlock(L);
}


/*
** 'load' and 'call' functions (run Lua code)
*/


#define checkresults(L,na,nr) \
     api_check(L, (nr) == LUA_MULTRET || (L->ci->top - L->top >= (nr) - (na)), \
	"results from function overflow current stack size")


LUA_API void lua_callk (lua_State *L, int nargs, int nresults,
                        lua_KContext ctx, lua_KFunction k) {
  StkId func;
  lua_lock(L);
  api_check(L, k == NULL || !isLua(L->ci),
    "cannot use continuations inside hooks");
  api_checknelems(L, nargs+1);
  api_check(L, L->status == LUA_OK, "cannot do calls on non-normal thread");
  checkresults(L, nargs, nresults);
  func = L->top - (nargs+1);
  if (k != NULL && L->nny == 0) {  /* need to prepare continuation? */
    L->ci->u.c.k = k;  /* save continuation */
    L->ci->u.c.ctx = ctx;  /* save context */
    luaD_call(L, func, nresults);  /* do the call */
  }
  else  /* no continuation or no yieldable */
    luaD_callnoyield(L, func, nresults);  /* just do the call */
  adjustresults(L, nresults);
  lua_unlock(L);
}



/*
** Execute a protected call.
*/
struct CallS {  /* data to 'f_call' */
  StkId func;
  int nresults;
};


static void f_call (lua_State *L, void *ud) {
  struct CallS *c = cast(struct CallS *, ud);
  luaD_callnoyield(L, c->func, c->nresults);
}



LUA_API int lua_pcallk (lua_State *L, int nargs, int nresults, int errfunc,
                        lua_KContext ctx, lua_KFunction k) {
  struct CallS c;
  int status;
  ptrdiff_t func;
  lua_lock(L);
  api_check(L, k == NULL || !isLua(L->ci),
    "cannot use continuations inside hooks");
  api_checknelems(L, nargs+1);
  api_check(L, L->status == LUA_OK, "cannot do calls on non-normal thread");
  checkresults(L, nargs, nresults);
  if (errfunc == 0)
    func = 0;
  else {
    StkId o = index2addr(L, errfunc);
    api_checkstackindex(L, errfunc, o);
    func = savestack(L, o);
  }
  c.func = L->top - (nargs+1);  /* function to be called */
  if (k == NULL || L->nny > 0) {  /* no continuation or no yieldable? */
    c.nresults = nresults;  /* do a 'conventional' protected call */
    status = luaD_pcall(L, f_call, &c, savestack(L, c.func), func);
  }
  else {  /* prepare continuation (call is already protected by 'resume') */
    CallInfo *ci = L->ci;
    ci->u.c.k = k;  /* save continuation */
    ci->u.c.ctx = ctx;  /* save context */
    /* save information for error recovery */
    ci->extra = savestack(L, c.func);
    ci->u.c.old_errfunc = L->errfunc;
    L->errfunc = func;
    setoah(ci->callstatus, L->allowhook);  /* save value of 'allowhook' */
    ci->callstatus |= CIST_YPCALL;  /* function can do error recovery */
    luaD_call(L, c.func, nresults);  /* do the call */
    ci->callstatus &= ~CIST_YPCALL;
    L->errfunc = ci->u.c.old_errfunc;
    status = LUA_OK;  /* if it is here, there were no errors */
  }
  adjustresults(L, nresults);
  lua_unlock(L);
  return status;
}


LUA_API int lua_load (lua_State *L, lua_Reader reader, void *data,
                      const char *chunkname, const char *mode) {
  ZIO z;
  int status;
  lua_lock(L);
  if (!chunkname) chunkname = "?";
  luaZ_init(L, &z, reader, data);
  status = luaD_protectedparser(L, &z, chunkname, mode);
  if (status == LUA_OK) {  /* no errors? */
    LClosure *f = clLvalue(L->top - 1);  /* get newly created function */
    if (f->nupvalues >= 1) {  /* does it have an upvalue? */
      /* get global table from registry */
      Table *reg = hvalue(&G(L)->l_registry);
      const TValue *gt = luaH_getint(reg, LUA_RIDX_GLOBALS);
      /* set global table as 1st upvalue of 'f' (may be LUA_ENV) */
      setobj(L, f->upvals[0]->v, gt);
      luaC_upvalbarrier(L, f->upvals[0]);
    }
  }
  lua_unlock(L);
  return status;
}


LUA_API int lua_dump (lua_State *L, lua_Writer writer, void *data, int strip) {
  int status;
  TValue *o;
  lua_lock(L);
  api_checknelems(L, 1);
  o = L->top - 1;
  if (isLfunction(o))
    status = luaU_dump(L, getproto(o), writer, data, strip);
  else
    status = 1;
  lua_unlock(L);
  return status;
}


LUA_API int lua_status (lua_State *L) {
  return L->status;
}


/*
** Garbage-collection function
*/

LUA_API int lua_gc (lua_State *L, int what, int data) {
  int res = 0;
  global_State *g;
  lua_lock(L);
  g = G(L);
  switch (what) {
    case LUA_GCSTOP: {
      g->gcrunning = 0;
      break;
    }
    case LUA_GCRESTART: {
      luaE_setdebt(g, 0);
      g->gcrunning = 1;
      break;
    }
    case LUA_GCCOLLECT: {
      luaC_fullgc(L, 0);
      break;
    }
    case LUA_GCCOUNT: {
      /* GC values are expressed in Kbytes: #bytes/2^10 */
      res = cast_int(gettotalbytes(g) >> 10);
      break;
    }
    case LUA_GCCOUNTB: {
      res = cast_int(gettotalbytes(g) & 0x3ff);
      break;
    }
    case LUA_GCSTEP: {
      l_mem debt = 1;  /* =1 to signal that it did an actual step */
      lu_byte oldrunning = g->gcrunning;
      g->gcrunning = 1;  /* allow GC to run */
      if (data == 0) {
        luaE_setdebt(g, -GCSTEPSIZE);  /* to do a "small" step */
        luaC_step(L);
      }
      else {  /* add 'data' to total debt */
        debt = cast(l_mem, data) * 1024 + g->GCdebt;
        luaE_setdebt(g, debt);
        luaC_checkGC(L);
      }
      g->gcrunning = oldrunning;  /* restore previous state */
      if (debt > 0 && g->gcstate == GCSpause)  /* end of cycle? */
        res = 1;  /* signal it */
      break;
    }
    case LUA_GCSETPAUSE: {
      res = g->gcpause;
      g->gcpause = data;
      break;
    }
    case LUA_GCSETSTEPMUL: {
      res = g->gcstepmul;
      if (data < 40) data = 40;  /* avoid ridiculous low values (and 0) */
      g->gcstepmul = data;
      break;
    }
    case LUA_GCISRUNNING: {
      res = g->gcrunning;
      break;
    }
    default: res = -1;  /* invalid option */
  }
  lua_unlock(L);
  return res;
}



/*
** miscellaneous functions
*/


LUA_API int lua_error (lua_State *L) {
  lua_lock(L);
  api_checknelems(L, 1);
  luaG_errormsg(L);
  /* code unreachable; will unlock when control actually leaves the kernel */
  return 0;  /* to avoid warnings */
}


LUA_API int lua_next (lua_State *L, int idx) {
  StkId t;
  int more;
  lua_lock(L);
  t = index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  more = luaH_next(L, hvalue(t), L->top - 1);
  if (more) {
    api_incr_top(L);
  }
  else  /* no more elements */
    L->top -= 1;  /* remove key */
  lua_unlock(L);
  return more;
}


LUA_API void lua_concat (lua_State *L, int n) {
  lua_lock(L);
  api_checknelems(L, n);
  if (n >= 2) {
    luaC_checkGC(L);
    luaV_concat(L, n);
  }
  else if (n == 0) {  /* push empty string */
    setsvalue2s(L, L->top, luaS_newlstr(L, "", 0));
    api_incr_top(L);
  }
  /* else n == 1; nothing to do */
  lua_unlock(L);
}


LUA_API void lua_len (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  t = index2addr(L, idx);
  luaV_objlen(L, L->top, t);
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API lua_Alloc lua_getallocf (lua_State *L, void **ud) {
  lua_Alloc f;
  lua_lock(L);
  if (ud) *ud = G(L)->ud;
  f = G(L)->frealloc;
  lua_unlock(L);
  return f;
}


LUA_API void lua_setallocf (lua_State *L, lua_Alloc f, void *ud) {
  lua_lock(L);
  G(L)->ud = ud;
  G(L)->frealloc = f;
  lua_unlock(L);
}


LUA_API void *lua_newuserdata (lua_State *L, size_t size) {
  Udata *u;
  lua_lock(L);
  luaC_checkGC(L);
  u = luaS_newudata(L, size);
  setuvalue(L, L->top, u);
  api_incr_top(L);
  lua_unlock(L);
  return getudatamem(u);
}



static const char *aux_upvalue (StkId fi, int n, TValue **val,
                                CClosure **owner, UpVal **uv, ravitype_t *type) {
  *type = RAVI_TANY;
  switch (ttype(fi)) {
    case LUA_TCCL: {  /* C closure */
      CClosure *f = clCvalue(fi);
      if (!(1 <= n && n <= f->nupvalues)) return NULL;
      *val = &f->upvalue[n-1];
      if (owner) *owner = f;
      return "";
    }
    case LUA_TLCL: {  /* Lua closure */
      LClosure *f = clLvalue(fi);
      TString *name;
      Proto *p = f->p;
      if (!(1 <= n && n <= p->sizeupvalues)) return NULL;
      *val = f->upvals[n-1]->v;
      if (uv) *uv = f->upvals[n - 1];
      name = p->upvalues[n-1].name;
      *type = p->upvalues[n - 1].type;
      return (name == NULL) ? "(*no name)" : getstr(name);
    }
    default: return NULL;  /* not a closure */
  }
}


LUA_API const char *lua_getupvalue (lua_State *L, int funcindex, int n) {
  const char *name;
  ravitype_t type;
  TValue *val = NULL;  /* to avoid warnings */
  lua_lock(L);
  name = aux_upvalue(index2addr(L, funcindex), n, &val, NULL, NULL, &type);
  if (name) {
    setobj2s(L, L->top, val);
    api_incr_top(L);
  }
  lua_unlock(L);
  return name;
}


LUA_API const char *lua_setupvalue (lua_State *L, int funcindex, int n) {
  const char *name;
  TValue *val = NULL;  /* to avoid warnings */
  CClosure *owner = NULL;
  UpVal *uv = NULL;
  StkId fi;
  ravitype_t type; /* RAVI upvalue type will be obtained if possible */
  lua_lock(L);
  fi = index2addr(L, funcindex);
  api_checknelems(L, 1);
  name = aux_upvalue(fi, n, &val, &owner, &uv, &type);
  if (name) {
    /* RAVI extension
    ** We need to ensure that this function does
    ** not subvert the types of local variables
    */
    if (type == RAVI_TNUMFLT || type == RAVI_TNUMINT || type == RAVI_TARRAYFLT || type == RAVI_TARRAYINT) {
      StkId input = L->top - 1;
      int compatible = (type == RAVI_TNUMFLT && ttisfloat(input))
        || (type == RAVI_TNUMINT && ttisinteger(input))
        || (type == RAVI_TARRAYFLT && ttistable(input) && hvalue(input)->ravi_array.array_type == RAVI_TARRAYFLT)
        || (type == RAVI_TARRAYINT && ttistable(input) && hvalue(input)->ravi_array.array_type == RAVI_TARRAYINT)
        || (type == RAVI_TTABLE && ttistable(input) && hvalue(input)->ravi_array.array_type == RAVI_TTABLE)
        ;
      if (!compatible)
        name = NULL;
    }
  }
  if (name) {
    L->top--;
    setobj(L, val, L->top);
    if (owner) { luaC_barrier(L, owner, L->top); }
    else if (uv) { luaC_upvalbarrier(L, uv); }
  }
  lua_unlock(L);
  return name;
}


static UpVal **getupvalref (lua_State *L, int fidx, int n, LClosure **pf) {
  LClosure *f;
  StkId fi = index2addr(L, fidx);
  api_check(L, ttisLclosure(fi), "Lua function expected");
  f = clLvalue(fi);
  api_check(L, (1 <= n && n <= f->p->sizeupvalues), "invalid upvalue index");
  if (pf) *pf = f;
  return &f->upvals[n - 1];  /* get its upvalue pointer */
}


LUA_API void *lua_upvalueid (lua_State *L, int fidx, int n) {
  StkId fi = index2addr(L, fidx);
  switch (ttype(fi)) {
    case LUA_TLCL: {  /* lua closure */
      return *getupvalref(L, fidx, n, NULL);
    }
    case LUA_TCCL: {  /* C closure */
      CClosure *f = clCvalue(fi);
      api_check(L, 1 <= n && n <= f->nupvalues, "invalid upvalue index");
      return &f->upvalue[n - 1];
    }
    default: {
      api_check(L, 0, "closure expected");
      return NULL;
    }
  }
}


LUA_API void lua_upvaluejoin (lua_State *L, int fidx1, int n1,
                                            int fidx2, int n2) {
  LClosure *f1;
  UpVal **up1 = getupvalref(L, fidx1, n1, &f1);
  UpVal **up2 = getupvalref(L, fidx2, n2, NULL);
  luaC_upvdeccount(L, *up1);
  *up1 = *up2;
  (*up1)->refcount++;
  if (upisopen(*up1)) (*up1)->u.open.touched = 1;
  luaC_upvalbarrier(L, *up1);
}


