/*
** $Id: lvm.c,v 2.265 2015/11/23 11:30:45 roberto Exp $
** Lua virtual machine
** See Copyright Notice in lua.h
*/

/*
** Portions Copyright (C) 2015 Dibyendu Majumdar
*/


#define lvm_c
#define LUA_CORE

#include "lprefix.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lvm.h"


/* limit for table tag-method chains (to avoid loops) */
#define MAXTAGLOOP	2000



/*
** 'l_intfitsf' checks whether a given integer can be converted to a
** float without rounding. Used in comparisons. Left undefined if
** all integers fit in a float precisely.
*/
#if !defined(l_intfitsf)

/* number of bits in the mantissa of a float */
#define NBM		(l_mathlim(MANT_DIG))

/*
** Check whether some integers may not fit in a float, that is, whether
** (maxinteger >> NBM) > 0 (that implies (1 << NBM) <= maxinteger).
** (The shifts are done in parts to avoid shifting by more than the size
** of an integer. In a worst case, NBM == 113 for long double and
** sizeof(integer) == 32.)
*/
#if ((((LUA_MAXINTEGER >> (NBM / 4)) >> (NBM / 4)) >> (NBM / 4)) \
	>> (NBM - (3 * (NBM / 4))))  >  0

#define l_intfitsf(i)  \
  (-((lua_Integer)1 << NBM) <= (i) && (i) <= ((lua_Integer)1 << NBM))

#endif

#endif



/*
** Try to convert a value to a float. The float case is already handled
** by the macro 'tonumber'.
*/
int luaV_tonumber_ (const TValue *obj, lua_Number *n) {
  TValue v;
  if (ttisinteger(obj)) {
    *n = cast_num(ivalue(obj));
    return 1;
  }
  else if (cvt2num(obj) &&  /* string convertible to number? */
            luaO_str2num(svalue(obj), &v) == vslen(obj) + 1) {
    *n = nvalue(&v);  /* convert result of 'luaO_str2num' to a float */
    return 1;
  }
  else
    return 0;  /* conversion failed */
}


/*
** try to convert a value to an integer, rounding according to 'mode':
** mode == 0: accepts only integral values
** mode == 1: takes the floor of the number
** mode == 2: takes the ceil of the number
*/
int luaV_tointeger (const TValue *obj, lua_Integer *p, int mode) {
  TValue v;
 again:
  if (ttisfloat(obj)) {
    lua_Number n = fltvalue(obj);
    lua_Number f = l_floor(n);
    if (n != f) {  /* not an integral value? */
      if (mode == 0) return 0;  /* fails if mode demands integral value */
      else if (mode > 1)  /* needs ceil? */
        f += 1;  /* convert floor to ceil (remember: n != f) */
    }
    return lua_numbertointeger(f, p);
  }
  else if (ttisinteger(obj)) {
    *p = ivalue(obj);
    return 1;
  }
  else if (cvt2num(obj) &&
            luaO_str2num(svalue(obj), &v) == vslen(obj) + 1) {
    obj = &v;
    goto again;  /* convert result from 'luaO_str2num' to an integer */
  }
  return 0;  /* conversion failed */
}


/*
** try to convert a value to an integer
*/
int luaV_tointeger_ (const TValue *obj, lua_Integer *p) {
  return luaV_tointeger(obj, p, LUA_FLOORN2I);
}


/*
** Try to convert a 'for' limit to an integer, preserving the
** semantics of the loop.
** (The following explanation assumes a non-negative step; it is valid
** for negative steps mutatis mutandis.)
** If the limit can be converted to an integer, rounding down, that is
** it.
** Otherwise, check whether the limit can be converted to a number.  If
** the number is too large, it is OK to set the limit as LUA_MAXINTEGER,
** which means no limit.  If the number is too negative, the loop
** should not run, because any initial integer value is larger than the
** limit. So, it sets the limit to LUA_MININTEGER. 'stopnow' corrects
** the extreme case when the initial value is LUA_MININTEGER, in which
** case the LUA_MININTEGER limit would still run the loop once.
*/
int luaV_forlimit (const TValue *obj, lua_Integer *p, lua_Integer step,
                     int *stopnow) {
  *stopnow = 0;  /* usually, let loops run */
  if (!luaV_tointeger(obj, p, (step < 0 ? 2 : 1))) {  /* not fit in integer? */
    lua_Number n;  /* try to convert to float */
    if (!tonumber(obj, &n)) /* cannot convert to float? */
      return 0;  /* not a number */
    if (luai_numlt(0, n)) {  /* if true, float is larger than max integer */
      *p = LUA_MAXINTEGER;
      if (step < 0) *stopnow = 1;
    }
    else {  /* float is smaller than min integer */
      *p = LUA_MININTEGER;
      if (step >= 0) *stopnow = 1;
    }
  }
  return 1;
}


/*
** Complete a table access: if 't' is a table, 'tm' has its metamethod;
** otherwise, 'tm' is NULL.
*/
void luaV_finishget (lua_State *L, const TValue *t, TValue *key, StkId val,
                      const TValue *tm) {
  int loop;  /* counter to avoid infinite loops */
  lua_assert(tm != NULL || !ttistable(t));
  for (loop = 0; loop < MAXTAGLOOP; loop++) {
    if (tm == NULL) {  /* no metamethod (from a table)? */
      if (ttisnil(tm = luaT_gettmbyobj(L, t, TM_INDEX)))
        luaG_typeerror(L, t, "index");  /* no metamethod */
    }
    if (ttisfunction(tm)) {  /* metamethod is a function */
      luaT_callTM(L, tm, t, key, val, 1);  /* call it */
      return;
    }
    t = tm;  /* else repeat access over 'tm' */
    if (luaV_fastget(L,t,key,tm,luaH_get)) {  /* try fast track */
      setobj2s(L, val, tm);  /* done */
      return;
    }
    /* else repeat */
  }
  luaG_runerror(L, "gettable chain too long; possible loop");
}


/*
** Main function for table assignment (invoking metamethods if needed).
** Compute 't[key] = val'
*/
void luaV_finishset (lua_State *L, const TValue *t, TValue *key,
                     StkId val, const TValue *oldval) {
  int loop;  /* counter to avoid infinite loops */
  for (loop = 0; loop < MAXTAGLOOP; loop++) {
    const TValue *tm;
    if (oldval != NULL) {
      lua_assert(ttistable(t) && ttisnil(oldval));
      Table *h = hvalue(t);  /* save 't' table */
      lua_assert(ttisnil(oldval));
      /* must check the metamethod */
      if ((tm = fasttm(L, h->metatable, TM_NEWINDEX)) == NULL &&
         /* no metamethod; is there a previous entry in the table? */
         (oldval != luaO_nilobject ||
         /* no previous entry; must create one. (The next test is
            always true; we only need the assignment.) */
         (oldval = luaH_newkey(L, h, key), 1))) {
        /* no metamethod and (now) there is an entry with given key */
        setobj2t(L, cast(TValue *, oldval), val);
        invalidateTMcache(h);
        luaC_barrierback(L, h, val);
        return;
      }
      /* else will try the metamethod */
    }
    else {  /* not a table; check metamethod */
      if (ttisnil(tm = luaT_gettmbyobj(L, t, TM_NEWINDEX)))
        luaG_typeerror(L, t, "index");
    }
    /* try the metamethod */
    if (ttisfunction(tm)) {
      luaT_callTM(L, tm, t, key, val, 0);
      return;
    }
    t = tm;  /* else repeat assignment over 'tm' */
    if (luaV_fastset(L, t, key, oldval, luaH_get, val))
      return;  /* done */
    /* else loop */
  }
  luaG_runerror(L, "settable chain too long; possible loop");
}

#define GETTABLE_INLINE(L, t, key, val) \
  if (!ttistable(t) || hvalue(t)->ravi_array.array_type == RAVI_TTABLE) { \
    const TValue *aux; \
    if (luaV_fastget(L,t,key,aux,luaH_get)) { setobj2s(L, val, aux); } \
    else luaV_finishget(L,t,key,val,aux); \
  } \
  else { \
    Table *h = hvalue(t); \
    if (h->ravi_array.array_type == RAVI_TARRAYFLT) { \
      if (!ttisinteger(key)) luaG_typeerror(L, key, "index"); \
      raviH_get_float_inline(L, h, ivalue(key), val); \
    } \
    else { \
      if (!ttisinteger(key)) luaG_typeerror(L, key, "index"); \
      raviH_get_int_inline(L, h, ivalue(key), val); \
    } \
  }


#define SETTABLE_INLINE(L, t, key, val) \
  if (!ttistable(t) || hvalue(t)->ravi_array.array_type == RAVI_TTABLE) { \
    const TValue *slot; \
    if (!luaV_fastset(L, t, key, slot, luaH_get, val)) \
      luaV_finishset(L, t, key, val, slot); \
  } \
  else { \
    Table *h = hvalue(t); \
    if (h->ravi_array.array_type == RAVI_TARRAYFLT) { \
      if (!ttisinteger(key)) luaG_typeerror(L, key, "index"); \
      if (ttisfloat(val)) { \
        raviH_set_float_inline(L, h, ivalue(key), fltvalue(val)); \
      } \
      else if (ttisinteger(val)) { \
        raviH_set_float_inline(L, h, ivalue(key), (lua_Number)(ivalue(val))); \
      } \
      else { \
        lua_Number d = 0.0; \
        if (luaV_tonumber_(val, &d)) { \
          raviH_set_float_inline(L, h, ivalue(key), d); \
        } \
        else \
          luaG_runerror(L, "value cannot be converted to number"); \
      } \
    } \
    else { \
      if (!ttisinteger(key)) luaG_typeerror(L, key, "index"); \
      if (ttisinteger(val)) { \
        raviH_set_int_inline(L, h, ivalue(key), ivalue(val)); \
      } \
      else { \
        lua_Integer i = 0; \
        if (luaV_tointeger_(val, &i)) { \
          raviH_set_int_inline(L, h, ivalue(key), i); \
        } \
        else \
          luaG_runerror(L, "value cannot be converted to integer"); \
      } \
    } \
  }

/*
** Main function for table access (invoking metamethods if needed).
** Compute 'val = t[key]'
** In Lua 5.3.2 this function is a macro but we need it to be a function
** so that JIT code can invoke it
*/
void luaV_gettable (lua_State *L, const TValue *t, TValue *key, StkId val) {
  GETTABLE_INLINE(L, t, key, val);
}


/*
** Main function for table assignment (invoking metamethods if needed).
** Compute 't[key] = val'
** In Lua 5.3.2 this function is a macro but we need it to be a function
** so that JIT code can invoke it
*/
void luaV_settable (lua_State *L, const TValue *t, TValue *key, StkId val) {
  SETTABLE_INLINE(L, t, key, val);
}


/*
** Compare two strings 'ls' x 'rs', returning an integer smaller-equal-
** -larger than zero if 'ls' is smaller-equal-larger than 'rs'.
** The code is a little tricky because it allows '\0' in the strings
** and it uses 'strcoll' (to respect locales) for each segments
** of the strings.
*/
static int l_strcmp (const TString *ls, const TString *rs) {
  const char *l = getstr(ls);
  size_t ll = tsslen(ls);
  const char *r = getstr(rs);
  size_t lr = tsslen(rs);
  for (;;) {  /* for each segment */
    int temp = strcoll(l, r);
    if (temp != 0)  /* not equal? */
      return temp;  /* done */
    else {  /* strings are equal up to a '\0' */
      size_t len = strlen(l);  /* index of first '\0' in both strings */
      if (len == lr)  /* 'rs' is finished? */
        return (len == ll) ? 0 : 1;  /* check 'ls' */
      else if (len == ll)  /* 'ls' is finished? */
        return -1;  /* 'ls' is smaller than 'rs' ('rs' is not finished) */
      /* both strings longer than 'len'; go on comparing after the '\0' */
      len++;
      l += len; ll -= len; r += len; lr -= len;
    }
  }
}


/*
** Check whether integer 'i' is less than float 'f'. If 'i' has an
** exact representation as a float ('l_intfitsf'), compare numbers as
** floats. Otherwise, if 'f' is outside the range for integers, result
** is trivial. Otherwise, compare them as integers. (When 'i' has no
** float representation, either 'f' is "far away" from 'i' or 'f' has
** no precision left for a fractional part; either way, how 'f' is
** truncated is irrelevant.) When 'f' is NaN, comparisons must result
** in false.
*/
static int LTintfloat (lua_Integer i, lua_Number f) {
#if defined(l_intfitsf)
  if (!l_intfitsf(i)) {
    if (f >= -cast_num(LUA_MININTEGER))  /* -minint == maxint + 1 */
      return 1;  /* f >= maxint + 1 > i */
    else if (f > cast_num(LUA_MININTEGER))  /* minint < f <= maxint ? */
      return (i < cast(lua_Integer, f));  /* compare them as integers */
    else  /* f <= minint <= i (or 'f' is NaN)  -->  not(i < f) */
      return 0;
  }
#endif
  return luai_numlt(cast_num(i), f);  /* compare them as floats */
}


/*
** Check whether integer 'i' is less than or equal to float 'f'.
** See comments on previous function.
*/
static int LEintfloat (lua_Integer i, lua_Number f) {
#if defined(l_intfitsf)
  if (!l_intfitsf(i)) {
    if (f >= -cast_num(LUA_MININTEGER))  /* -minint == maxint + 1 */
      return 1;  /* f >= maxint + 1 > i */
    else if (f >= cast_num(LUA_MININTEGER))  /* minint <= f <= maxint ? */
      return (i <= cast(lua_Integer, f));  /* compare them as integers */
    else  /* f < minint <= i (or 'f' is NaN)  -->  not(i <= f) */
      return 0;
  }
#endif
  return luai_numle(cast_num(i), f);  /* compare them as floats */
}


/*
** Return 'l < r', for numbers.
*/
static int LTnum (const TValue *l, const TValue *r) {
  if (ttisinteger(l)) {
    lua_Integer li = ivalue(l);
    if (ttisinteger(r))
      return li < ivalue(r);  /* both are integers */
    else  /* 'l' is int and 'r' is float */
      return LTintfloat(li, fltvalue(r));  /* l < r ? */
  }
  else {
    lua_Number lf = fltvalue(l);  /* 'l' must be float */
    if (ttisfloat(r))
      return luai_numlt(lf, fltvalue(r));  /* both are float */
    else if (luai_numisnan(lf))  /* 'r' is int and 'l' is float */
      return 0;  /* NaN < i is always false */
    else  /* without NaN, (l < r)  <-->  not(r <= l) */
      return !LEintfloat(ivalue(r), lf);  /* not (r <= l) ? */
  }
}


/*
** Return 'l <= r', for numbers.
*/
static int LEnum (const TValue *l, const TValue *r) {
  if (ttisinteger(l)) {
    lua_Integer li = ivalue(l);
    if (ttisinteger(r))
      return li <= ivalue(r);  /* both are integers */
    else  /* 'l' is int and 'r' is float */
      return LEintfloat(li, fltvalue(r));  /* l <= r ? */
  }
  else {
    lua_Number lf = fltvalue(l);  /* 'l' must be float */
    if (ttisfloat(r))
      return luai_numle(lf, fltvalue(r));  /* both are float */
    else if (luai_numisnan(lf))  /* 'r' is int and 'l' is float */
      return 0;  /*  NaN <= i is always false */
    else  /* without NaN, (l <= r)  <-->  not(r < l) */
      return !LTintfloat(ivalue(r), lf);  /* not (r < l) ? */
  }
}


/*
** Main operation less than; return 'l < r'.
*/
int luaV_lessthan (lua_State *L, const TValue *l, const TValue *r) {
  int res;
  if (ttisnumber(l) && ttisnumber(r))  /* both operands are numbers? */
    return LTnum(l, r);
  else if (ttisstring(l) && ttisstring(r))  /* both are strings? */
    return l_strcmp(tsvalue(l), tsvalue(r)) < 0;
  else if ((res = luaT_callorderTM(L, l, r, TM_LT)) < 0)  /* no metamethod? */
    luaG_ordererror(L, l, r);  /* error */
  return res;
}


/*
** Main operation less than or equal to; return 'l <= r'. If it needs
** a metamethod and there is no '__le', try '__lt', based on
** l <= r iff !(r < l) (assuming a total order). If the metamethod
** yields during this substitution, the continuation has to know
** about it (to negate the result of r<l); bit CIST_LEQ in the call
** status keeps that information.
*/
int luaV_lessequal (lua_State *L, const TValue *l, const TValue *r) {
  int res;
  if (ttisnumber(l) && ttisnumber(r))  /* both operands are numbers? */
    return LEnum(l, r);
  else if (ttisstring(l) && ttisstring(r))  /* both are strings? */
    return l_strcmp(tsvalue(l), tsvalue(r)) <= 0;
  else if ((res = luaT_callorderTM(L, l, r, TM_LE)) >= 0)  /* try 'le' */
    return res;
  else {  /* try 'lt': */
    L->ci->callstatus |= CIST_LEQ;  /* mark it is doing 'lt' for 'le' */
    res = luaT_callorderTM(L, r, l, TM_LT);
    L->ci->callstatus ^= CIST_LEQ;  /* clear mark */
    if (res < 0)
      luaG_ordererror(L, l, r);
    return !res;  /* result is negated */
  }
}


/*
** Main operation for equality of Lua values; return 't1 == t2'.
** L == NULL means raw equality (no metamethods)
*/
int luaV_equalobj (lua_State *L, const TValue *t1, const TValue *t2) {
  const TValue *tm;
  if (ttype(t1) != ttype(t2)) {  /* not the same variant? */
    if (ttnov(t1) != ttnov(t2) || ttnov(t1) != LUA_TNUMBER)
      return 0;  /* only numbers can be equal with different variants */
    else {  /* two numbers with different variants */
      lua_Integer i1, i2;  /* compare them as integers */
      return (tointeger(t1, &i1) && tointeger(t2, &i2) && i1 == i2);
    }
  }
  /* values have same type and same variant */
  switch (ttype(t1)) {
    case LUA_TNIL: return 1;
    case LUA_TNUMINT: return (ivalue(t1) == ivalue(t2));
    case LUA_TNUMFLT: return luai_numeq(fltvalue(t1), fltvalue(t2));
    case LUA_TBOOLEAN: return bvalue(t1) == bvalue(t2);  /* true must be 1 !! */
    case LUA_TLIGHTUSERDATA: return pvalue(t1) == pvalue(t2);
    case LUA_TLCF: return fvalue(t1) == fvalue(t2);
    case LUA_TSHRSTR: return eqshrstr(tsvalue(t1), tsvalue(t2));
    case LUA_TLNGSTR: return luaS_eqlngstr(tsvalue(t1), tsvalue(t2));
    case LUA_TUSERDATA: {
      if (uvalue(t1) == uvalue(t2)) return 1;
      else if (L == NULL) return 0;
      tm = fasttm(L, uvalue(t1)->metatable, TM_EQ);
      if (tm == NULL)
        tm = fasttm(L, uvalue(t2)->metatable, TM_EQ);
      break;  /* will try TM */
    }
    case LUA_TTABLE: {
      if (hvalue(t1) == hvalue(t2)) return 1;
      else if (L == NULL) return 0;
      tm = fasttm(L, hvalue(t1)->metatable, TM_EQ);
      if (tm == NULL)
        tm = fasttm(L, hvalue(t2)->metatable, TM_EQ);
      break;  /* will try TM */
    }
    default:
      return gcvalue(t1) == gcvalue(t2);
  }
  if (tm == NULL)  /* no TM? */
    return 0;  /* objects are different */
  luaT_callTM(L, tm, t1, t2, L->top, 1);  /* call TM */
  return !l_isfalse(L->top);
}


/* macro used by 'luaV_concat' to ensure that element at 'o' is a string */
#define tostring(L,o)  \
	(ttisstring(o) || (cvt2str(o) && (luaO_tostring(L, o), 1)))

#define isemptystr(o)	(ttisshrstring(o) && tsvalue(o)->shrlen == 0)

/* copy strings in stack from top - n up to top - 1 to buffer */
static void copy2buff (StkId top, int n, char *buff) {
  size_t tl = 0;  /* size already copied */
  do {
    size_t l = vslen(top - n);  /* length of string being copied */
    memcpy(buff + tl, svalue(top - n), l * sizeof(char));
    tl += l;
  } while (--n > 0);
}


/*
** Main operation for concatenation: concat 'total' values in the stack,
** from 'L->top - total' up to 'L->top - 1'.
*/
void luaV_concat (lua_State *L, int total) {
  lua_assert(total >= 2);
  do {
    StkId top = L->top;
    int n = 2;  /* number of elements handled in this pass (at least 2) */
    if (!(ttisstring(top-2) || cvt2str(top-2)) || !tostring(L, top-1))
      luaT_trybinTM(L, top-2, top-1, top-2, TM_CONCAT);
    else if (isemptystr(top - 1))  /* second operand is empty? */
      cast_void(tostring(L, top - 2));  /* result is first operand */
    else if (isemptystr(top - 2)) {  /* first operand is an empty string? */
      setobjs2s(L, top - 2, top - 1);  /* result is second op. */
    }
    else {
      /* at least two non-empty string values; get as many as possible */
      size_t tl = vslen(top - 1);
      TString *ts;
      /* collect total length and number of strings */
      for (n = 1; n < total && tostring(L, top - n - 1); n++) {
        size_t l = vslen(top - n - 1);
        if (l >= (MAX_SIZE/sizeof(char)) - tl)
          luaG_runerror(L, "string length overflow");
        tl += l;
      }
      if (tl <= LUAI_MAXSHORTLEN) {  /* is result a short string? */
        char buff[LUAI_MAXSHORTLEN];
        copy2buff(top, n, buff);  /* copy strings to buffer */
        ts = luaS_newlstr(L, buff, tl);
      }
      else {  /* long string; copy strings directly to final result */
        ts = luaS_createlngstrobj(L, tl);
        copy2buff(top, n, getstr(ts));
      }
      setsvalue2s(L, top - n, ts);  /* create result */
    }
    total -= n-1;  /* got 'n' strings to create 1 new */
    L->top -= n-1;  /* popped 'n' strings and pushed one */
  } while (total > 1);  /* repeat until only 1 result left */
}


/*
** Main operation 'ra' = #rb'.
*/
void luaV_objlen (lua_State *L, StkId ra, const TValue *rb) {
  const TValue *tm;
  switch (ttype(rb)) {
    case LUA_TTABLE: {
      Table *h = hvalue(rb);
      if (h->ravi_array.array_type != RAVI_TTABLE) {
        setivalue(ra, raviH_getn(h));
        return;
      }
      else {
        tm = fasttm(L, h->metatable, TM_LEN);
        if (tm) break;  /* metamethod? break switch to call it */
        setivalue(ra, luaH_getn(h));  /* else primitive len */
        return;
      }
    }
    case LUA_TSHRSTR: {
      setivalue(ra, tsvalue(rb)->shrlen);
      return;
    }
    case LUA_TLNGSTR: {
      setivalue(ra, tsvalue(rb)->u.lnglen);
      return;
    }
    default: {  /* try metamethod */
      tm = luaT_gettmbyobj(L, rb, TM_LEN);
      if (ttisnil(tm))  /* no metamethod? */
        luaG_typeerror(L, rb, "get length of");
      break;
    }
  }
  luaT_callTM(L, tm, rb, rb, ra, 1);
}


/*
** Integer division; return 'm // n', that is, floor(m/n).
** C division truncates its result (rounds towards zero).
** 'floor(q) == trunc(q)' when 'q >= 0' or when 'q' is integer,
** otherwise 'floor(q) == trunc(q) - 1'.
*/
lua_Integer luaV_div (lua_State *L, lua_Integer m, lua_Integer n) {
  if (l_castS2U(n) + 1u <= 1u) {  /* special cases: -1 or 0 */
    if (n == 0)
      luaG_runerror(L, "attempt to divide by zero");
    return intop(-, 0, m);   /* n==-1; avoid overflow with 0x80000...//-1 */
  }
  else {
    lua_Integer q = m / n;  /* perform C division */
    if ((m ^ n) < 0 && m % n != 0)  /* 'm/n' would be negative non-integer? */
      q -= 1;  /* correct result for different rounding */
    return q;
  }
}


/*
** Integer modulus; return 'm % n'. (Assume that C '%' with
** negative operands follows C99 behavior. See previous comment
** about luaV_div.)
*/
lua_Integer luaV_mod (lua_State *L, lua_Integer m, lua_Integer n) {
  if (l_castS2U(n) + 1u <= 1u) {  /* special cases: -1 or 0 */
    if (n == 0)
      luaG_runerror(L, "attempt to perform 'n%%0'");
    return 0;   /* m % -1 == 0; avoid overflow with 0x80000...%-1 */
  }
  else {
    lua_Integer r = m % n;
    if (r != 0 && (m ^ n) < 0)  /* 'm/n' would be non-integer negative? */
      r += n;  /* correct result for different rounding */
    return r;
  }
}


/* number of bits in an integer */
#define NBITS	cast_int(sizeof(lua_Integer) * CHAR_BIT)

/*
** Shift left operation. (Shift right just negates 'y'.)
*/
lua_Integer luaV_shiftl (lua_Integer x, lua_Integer y) {
  if (y < 0) {  /* shift right? */
    if (y <= -NBITS) return 0;
    else return intop(>>, x, -y);
  }
  else {  /* shift left */
    if (y >= NBITS) return 0;
    else return intop(<<, x, y);
  }
}


/*
** check whether cached closure in prototype 'p' may be reused, that is,
** whether there is a cached closure with the same upvalues needed by
** new closure to be created.
*/
static LClosure *getcached (Proto *p, UpVal **encup, StkId base) {
  LClosure *c = p->cache;
  if (c != NULL) {  /* is there a cached closure? */
    int nup = p->sizeupvalues;
    Upvaldesc *uv = p->upvalues;
    int i;
    for (i = 0; i < nup; i++) {  /* check whether it has right upvalues */
      TValue *v = uv[i].instack ? base + uv[i].idx : encup[uv[i].idx]->v;
      if (c->upvals[i]->v != v)
        return NULL;  /* wrong upvalue; cannot reuse closure */
    }
  }
  return c;  /* return cached closure (or NULL if no cached closure) */
}


/*
** create a new Lua closure, push it in the stack, and initialize
** its upvalues. Note that the closure is not cached if prototype is
** already black (which means that 'cache' was already cleared by the
** GC).
*/
static void pushclosure (lua_State *L, Proto *p, UpVal **encup, StkId base,
                         StkId ra) {
  int nup = p->sizeupvalues;
  Upvaldesc *uv = p->upvalues;
  int i;
  LClosure *ncl = luaF_newLclosure(L, nup);
  ncl->p = p;
  setclLvalue(L, ra, ncl);  /* anchor new closure in stack */
  for (i = 0; i < nup; i++) {  /* fill in its upvalues */
    if (uv[i].instack)  /* upvalue refers to local variable? */
      ncl->upvals[i] = luaF_findupval(L, base + uv[i].idx);
    else  /* get upvalue from enclosing function */
      ncl->upvals[i] = encup[uv[i].idx];
    ncl->upvals[i]->refcount++;
    /* new closure is white, so we do not need a barrier here */
  }
  if (!isblack(p))  /* cache will not break GC invariant? */
    p->cache = ncl;  /* save it on cache for reuse */
}


/*
** finish execution of an opcode interrupted by an yield
*/
void luaV_finishOp (lua_State *L) {
  CallInfo *ci = L->ci;
  StkId base = ci->u.l.base;
  Instruction inst = *(ci->u.l.savedpc - 1);  /* interrupted instruction */
  OpCode op = GET_OPCODE(inst);
  switch (op) {  /* finish its execution */
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_IDIV:
    case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR:
    case OP_MOD: case OP_POW:
    case OP_UNM: case OP_BNOT: case OP_LEN:
    case OP_GETTABUP: case OP_GETTABLE: case OP_SELF: {
      setobjs2s(L, base + GETARG_A(inst), --L->top);
      break;
    }
    case OP_LE: case OP_LT: case OP_EQ: {
      int res = !l_isfalse(L->top - 1);
      L->top--;
      if (ci->callstatus & CIST_LEQ) {  /* "<=" using "<" instead? */
        lua_assert(op == OP_LE);
        ci->callstatus ^= CIST_LEQ;  /* clear mark */
        res = !res;  /* negate result */
      }
      lua_assert(GET_OPCODE(*ci->u.l.savedpc) == OP_JMP);
      if (res != GETARG_A(inst))  /* condition failed? */
        ci->u.l.savedpc++;  /* skip jump instruction */
      break;
    }
    case OP_CONCAT: {
      StkId top = L->top - 1;  /* top when 'luaT_trybinTM' was called */
      int b = GETARG_B(inst);      /* first element to concatenate */
      int total = cast_int(top - 1 - (base + b));  /* yet to concatenate */
      setobj2s(L, top - 2, top);  /* put TM result in proper position */
      if (total > 1) {  /* are there elements to concat? */
        L->top = top - 1;  /* top is one after last element (at top-2) */
        luaV_concat(L, total);  /* concat them (may yield again) */
      }
      /* move final result to final position */
      setobj2s(L, ci->u.l.base + GETARG_A(inst), L->top - 1);
      L->top = ci->top;  /* restore top */
      break;
    }
    case OP_TFORCALL: {
      lua_assert(GET_OPCODE(*ci->u.l.savedpc) == OP_TFORLOOP);
      L->top = ci->top;  /* correct top */
      break;
    }
    case OP_CALL: {
      if (GETARG_C(inst) - 1 >= 0)  /* nresults >= 0? */
        L->top = ci->top;  /* adjust results */
      break;
    }
    case OP_TAILCALL: case OP_SETTABUP: case OP_SETTABLE:
      break;
    default: lua_assert(0);
  }
}




/*
** {==================================================================
** Function 'luaV_execute': main interpreter loop
** ===================================================================
*/


/*
** some macros for common tasks in 'luaV_execute'
*/


#define RA(i)	(base+GETARG_A(i))
#define RB(i)	check_exp(getBMode(GET_OPCODE(i)) == OpArgR, base+GETARG_B(i))
#define RC(i)	check_exp(getCMode(GET_OPCODE(i)) == OpArgR, base+GETARG_C(i))
#define RKB(i)	check_exp(getBMode(GET_OPCODE(i)) == OpArgK, \
	ISK(GETARG_B(i)) ? k+INDEXK(GETARG_B(i)) : base+GETARG_B(i))
#define RKC(i)	check_exp(getCMode(GET_OPCODE(i)) == OpArgK, \
	ISK(GETARG_C(i)) ? k+INDEXK(GETARG_C(i)) : base+GETARG_C(i))


/* execute a jump instruction */
#define dojump(ci,i,e) \
  { int a = GETARG_A(i); \
    if (a != 0) luaF_close(L, ci->u.l.base + a - 1); \
    ci->u.l.savedpc += GETARG_sBx(i) + e; }

/* for test instructions, execute the jump instruction that follows it */
#define donextjump(ci)	{ i = *ci->u.l.savedpc; dojump(ci, i, 1); }

/* when executing code that could potentially reallocate the stack
 * and thereby invalidate the cached value of 'base' then it needs to 
 * be restored - the Protect macros achieves that
 */
#define Protect(x)	{ {x;}; base = ci->u.l.base; }

#define checkGC(L,c)  \
	{ luaC_condGC(L, L->top = (c),  /* limit of live values */ \
                         Protect(L->top = ci->top));  /* restore top */ \
           luai_threadyield(L); }

#define checkGC_(L,c)  \
    { luaC_condGC(L, L->top = (c),  /* limit of live values */ \
                     L->top = ci->top);  /* restore top */ \
           luai_threadyield(L); }


#define vmdispatch(o)	switch(o)
#define vmcase(l)	case l:
#define vmbreak		break


/*
** copy of 'luaV_gettable', but protecting call to potential metamethod
** (which can reallocate the stack)
*/
#define gettableProtected(L,t,k,v)  { const TValue *aux; \
  if (luaV_fastget(L,t,k,aux,luaH_get)) { setobj2s(L, v, aux); } \
  else Protect(luaV_finishget(L,t,k,v,aux)); }


/* same for 'luaV_settable' */
#define settableProtected(L,t,k,v) { const TValue *slot; \
  if (!luaV_fastset(L,t,k,slot,luaH_get,v)) \
    Protect(luaV_finishset(L,t,k,v,slot)); }



int luaV_execute (lua_State *L) {
  CallInfo *ci = L->ci;
  LClosure *cl;
  TValue *k;
  StkId base;
  ci->callstatus |= CIST_FRESH;  /* fresh invocation of 'luaV_execute" */
 newframe:  /* reentry point when frame changes (call/return) */
  lua_assert(ci == L->ci);
  cl = clLvalue(ci->func);  /* local reference to function's closure */
  k = cl->p->k;  /* local reference to function's constant table */
  base = ci->u.l.base;  /* local copy of function's base */
  /* main loop of interpreter */
  for (;;) {
    Instruction i = *(ci->u.l.savedpc++);
    StkId ra;
    if (L->hookmask & (LUA_MASKLINE | LUA_MASKCOUNT))
      Protect(luaG_traceexec(L));
    /* WARNING: several calls may realloc the stack and invalidate 'ra' */
    OpCode op = GET_OPCODE(i);
#if 0
    RAVI_DEBUG_STACK(
        ravi_debug_trace(L, op, (ci->u.l.savedpc - cl->p->code) - 1));
#endif
    ra = RA(i);
    lua_assert(base == ci->u.l.base);
    lua_assert(base <= L->top && L->top < L->stack + L->stacksize);
    switch (op) {
      case OP_MOVE: {
        setobjs2s(L, ra, RB(i));
      } break;
      case OP_LOADK: {
        TValue *rb = k + GETARG_Bx(i);
        setobj2s(L, ra, rb);
      } break;
      case OP_LOADKX: {
        TValue *rb;
        lua_assert(GET_OPCODE(*ci->u.l.savedpc) == OP_EXTRAARG);
        rb = k + GETARG_Ax(*ci->u.l.savedpc++);
        setobj2s(L, ra, rb);
      } break;
      case OP_LOADBOOL: {
        setbvalue(ra, GETARG_B(i));
        if (GETARG_C(i)) ci->u.l.savedpc++;  /* skip next instruction (if C) */
      } break;
      case OP_LOADNIL: {
        int b = GETARG_B(i);
        do {
          setnilvalue(ra++);
        } while (b--);

      } break;
      case OP_GETUPVAL: {
        int b = GETARG_B(i);
        setobj2s(L, ra, cl->upvals[b]->v);
      } break;

      case OP_GETTABUP: {
        TValue *upval = cl->upvals[GETARG_B(i)]->v;    /* table */
        TValue *rc = RKC(i);                           /* key */
        GETTABLE_INLINE(L, upval, rc, ra);
        Protect((void)0);
      } break;
      case OP_GETTABLE: {
        StkId rb = RB(i);                              /* table */
        TValue *rc = RKC(i);                           /* key */
        GETTABLE_INLINE(L, rb, rc, ra);
        Protect((void)0);
      } break;

      case OP_SETUPVAL: {
        UpVal *uv = cl->upvals[GETARG_B(i)];
        setobj(L, uv->v, ra);
        luaC_upvalbarrier(L, uv);
      } break;

      case OP_RAVI_SETTABLE_S:
      case OP_RAVI_SETTABLE_I:
      case OP_SETTABUP:
      case OP_SETTABLE: {
        TValue *rb = RKB(i);
        TValue *t = (op == OP_SETTABUP) ? cl->upvals[GETARG_A(i)]->v : ra;
        TValue *rc = RKC(i);
        SETTABLE_INLINE(L, t, rb, rc);
        Protect((void)0);
      } break;

      case OP_NEWTABLE: {
        int b = GETARG_B(i);
        int c = GETARG_C(i);
        Table *t = luaH_new(L);
        sethvalue(L, ra, t);
        if (b != 0 || c != 0)
          luaH_resize(L, t, luaO_fb2int(b), luaO_fb2int(c));
        checkGC(L, ra + 1);
      } break;
      case OP_SELF: {
        const TValue *aux;
        StkId rb = RB(i);
        TValue *rc = RKC(i);
        TString *key = tsvalue(rc);  /* key must be a string */
        setobjs2s(L, ra + 1, rb);
        if (luaV_fastget(L, rb, key, aux, luaH_getstr)) {
          setobj2s(L, ra, aux);
        }
        else Protect(luaV_finishget(L, rb, rc, ra, aux));
      } break;
      case OP_ADD: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Number nb; lua_Number nc;
        if (ttisinteger(rb) && ttisinteger(rc)) {
          lua_Integer ib = ivalue(rb); lua_Integer ic = ivalue(rc);
          setivalue(ra, intop(+, ib, ic));
        }
        else if (tonumber(rb, &nb) && tonumber(rc, &nc)) {
          setfltvalue(ra, luai_numadd(L, nb, nc));
        }
        else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_ADD)); }
      } break;
      case OP_SUB: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Number nb; lua_Number nc;
        if (ttisinteger(rb) && ttisinteger(rc)) {
          lua_Integer ib = ivalue(rb); lua_Integer ic = ivalue(rc);
          setivalue(ra, intop(-, ib, ic));
        }
        else if (tonumber(rb, &nb) && tonumber(rc, &nc)) {
          setfltvalue(ra, luai_numsub(L, nb, nc));
        }
        else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_SUB)); }
      } break;
      case OP_MUL: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Number nb; lua_Number nc;
        if (ttisinteger(rb) && ttisinteger(rc)) {
          lua_Integer ib = ivalue(rb); lua_Integer ic = ivalue(rc);
          setivalue(ra, intop(*, ib, ic));
        }
        else if (tonumber(rb, &nb) && tonumber(rc, &nc)) {
          setfltvalue(ra, luai_nummul(L, nb, nc));
        }
        else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_MUL)); }
      } break;
      case OP_DIV: { /* float division (always with floats) */
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Number nb; lua_Number nc;
        if (tonumber(rb, &nb) && tonumber(rc, &nc)) {
          setfltvalue(ra, luai_numdiv(L, nb, nc));
        }
        else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_DIV)); }
      } break;
      case OP_BAND: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Integer ib; lua_Integer ic;
        if (tointeger(rb, &ib) && tointeger(rc, &ic)) {
          setivalue(ra, intop(&, ib, ic));
        }
        else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_BAND)); }
      } break;
      case OP_BOR: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Integer ib; lua_Integer ic;
        if (tointeger(rb, &ib) && tointeger(rc, &ic)) {
          setivalue(ra, intop(|, ib, ic));
        }
        else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_BOR)); }
      } break;
      case OP_BXOR: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Integer ib; lua_Integer ic;
        if (tointeger(rb, &ib) && tointeger(rc, &ic)) {
          setivalue(ra, intop(^, ib, ic));
        }
        else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_BXOR)); }
      } break;
      case OP_SHL: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Integer ib; lua_Integer ic;
        if (tointeger(rb, &ib) && tointeger(rc, &ic)) {
          setivalue(ra, luaV_shiftl(ib, ic));
        }
        else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_SHL)); }
      } break;
      case OP_SHR: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Integer ib; lua_Integer ic;
        if (tointeger(rb, &ib) && tointeger(rc, &ic)) {
          setivalue(ra, luaV_shiftl(ib, -ic));
        }
        else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_SHR)); }
      } break;
      case OP_MOD: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Number nb; lua_Number nc;
        if (ttisinteger(rb) && ttisinteger(rc)) {
          lua_Integer ib = ivalue(rb); lua_Integer ic = ivalue(rc);
          setivalue(ra, luaV_mod(L, ib, ic));
        }
        else if (tonumber(rb, &nb) && tonumber(rc, &nc)) {
          lua_Number m;
          luai_nummod(L, nb, nc, m);
          setfltvalue(ra, m);
        }
        else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_MOD)); }
      } break;
      case OP_IDIV: { /* floor division */
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Number nb; lua_Number nc;
        if (ttisinteger(rb) && ttisinteger(rc)) {
          lua_Integer ib = ivalue(rb); lua_Integer ic = ivalue(rc);
          setivalue(ra, luaV_div(L, ib, ic));
        }
        else if (tonumber(rb, &nb) && tonumber(rc, &nc)) {
          setfltvalue(ra, luai_numidiv(L, nb, nc));
        }
        else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_IDIV)); }
      } break;
      case OP_POW: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Number nb; lua_Number nc;
        if (tonumber(rb, &nb) && tonumber(rc, &nc)) {
          setfltvalue(ra, luai_numpow(L, nb, nc));
        }
        else { Protect(luaT_trybinTM(L, rb, rc, ra, TM_POW)); }
      } break;
      case OP_UNM: {
        TValue *rb = RB(i);
        lua_Number nb;
        if (ttisinteger(rb)) {
          lua_Integer ib = ivalue(rb);
          setivalue(ra, intop(-, 0, ib));
        }
        else if (tonumber(rb, &nb)) {
          setfltvalue(ra, luai_numunm(L, nb));
        }
        else {
          Protect(luaT_trybinTM(L, rb, rb, ra, TM_UNM));
        }
      } break;
      case OP_BNOT: {
        TValue *rb = RB(i);
        lua_Integer ib;
        if (tointeger(rb, &ib)) {
          setivalue(ra, intop(^, ~l_castS2U(0), ib));
        }
        else {
          Protect(luaT_trybinTM(L, rb, rb, ra, TM_BNOT));
        }
      } break;
      case OP_NOT: {
        TValue *rb = RB(i);
        int res = l_isfalse(rb);  /* next assignment may change this value */
        setbvalue(ra, res);
      } break;
      case OP_LEN: {
        Protect(luaV_objlen(L, ra, RB(i)));
      } break;
      case OP_CONCAT: {
        int b = GETARG_B(i);
        int c = GETARG_C(i);
        StkId rb;
        L->top = base + c + 1;  /* mark the end of concat operands */
        Protect(luaV_concat(L, c - b + 1));
        ra = RA(i);  /* 'luaV_concat' may invoke TMs and move the stack */
        rb = base + b;
        setobjs2s(L, ra, rb);
        checkGC(L, (ra >= rb ? ra + 1 : rb));
        L->top = ci->top;  /* restore top */
      } break;
      case OP_JMP: {
        dojump(ci, i, 0);
      } break;
      case OP_EQ: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        Protect(
          if (luaV_equalobj(L, rb, rc) != GETARG_A(i))
            ci->u.l.savedpc++;
          else
            donextjump(ci);
        )
      } break;
      case OP_LT: {
        Protect(
          if (luaV_lessthan(L, RKB(i), RKC(i)) != GETARG_A(i))
            ci->u.l.savedpc++;
          else
            donextjump(ci);
        )
      } break;
      case OP_LE: {
        Protect(
          if (luaV_lessequal(L, RKB(i), RKC(i)) != GETARG_A(i))
            ci->u.l.savedpc++;
          else
            donextjump(ci);
        )
      } break;
      case OP_TEST: {
        if (GETARG_C(i) ? l_isfalse(ra) : !l_isfalse(ra))
            ci->u.l.savedpc++;
        else
          donextjump(ci);
      } break;
      case OP_TESTSET: {
        TValue *rb = RB(i);
        if (GETARG_C(i) ? l_isfalse(rb) : !l_isfalse(rb))
          ci->u.l.savedpc++;
        else {
          setobjs2s(L, ra, rb);
          donextjump(ci);
        }
      } break;
      case OP_CALL: {
        int b = GETARG_B(i);
        int nresults = GETARG_C(i) - 1;
        if (b != 0) L->top = ra+b;  /* else previous instruction set top */
        /*
        See note below under OP_RETURN for why we pass the extra
        argument to luaD_precall() - it is basicaly to tell it that it
        was called from OP_CALL instruction
        */
        int c_or_compiled = luaD_precall(L, ra, nresults, 1 /* OP_CALL */);
        if (c_or_compiled) { /* C or Lua JITed function? */
          /* RAVI change - if the Lua function was JIT compiled then
           * luaD_precall() returns 2
           * A return value of 1 indicates non Lua C function
           */
          if (c_or_compiled == 1 && nresults >= 0) {
            lua_assert(ci == L->ci);
            L->top = ci->top; /* adjust results */
          }
          Protect((void)0);  /* update 'base' */
        }
        else { /* Lua function */
          ci = L->ci;
          lua_assert(!ci->jitstatus);
          goto newframe; /* restart luaV_execute over new Lua function */
        }
      } break;
      case OP_TAILCALL: {
        int b = GETARG_B(i);
        if (b != 0) L->top = ra+b;  /* else previous instruction set top */
        lua_assert(GETARG_C(i) - 1 == LUA_MULTRET);
        /* See note below under OP_RETURN for why we pass the extra
           argument to luaD_precall() - it is basicaly to tell it that it
           was called from OP_CALL instruction
           */
        if (luaD_precall(L, ra, LUA_MULTRET,
                         1 /* OP_CALL */)) { /* C function? or JIT call */
          Protect((void)0);  /* update 'base' */
        }
        else {
          /* tail call: put called frame (n) in place of caller one (o) */
          CallInfo *nci = L->ci;  /* called frame */
          CallInfo *oci = nci->previous;  /* caller frame */
          StkId nfunc = nci->func;  /* called function */
          StkId ofunc = oci->func;  /* caller function */
          /* last stack slot filled by 'precall' */
          StkId lim = nci->u.l.base + getproto(nfunc)->numparams;
          int aux;
          /* close all upvalues from previous call */
          if (cl->p->sizep > 0) luaF_close(L, oci->u.l.base);
          /* move new frame into old one */
          for (aux = 0; nfunc + aux < lim; aux++)
            setobjs2s(L, ofunc + aux, nfunc + aux);
          oci->u.l.base = ofunc + (nci->u.l.base - nfunc);  /* correct base */
          oci->top = L->top = ofunc + (L->top - nfunc);  /* correct top */
          oci->u.l.savedpc = nci->u.l.savedpc;
          oci->callstatus |= CIST_TAIL;  /* function was tail called */
          oci->jitstatus = 0;
          ci = L->ci = oci;  /* remove new frame */
          lua_assert(L->top == oci->u.l.base + getproto(ofunc)->maxstacksize);
          goto newframe;  /* restart luaV_execute over new Lua function */
        }
      } break;
      case OP_RETURN: {
        int b = GETARG_B(i);
        if (cl->p->sizep > 0) luaF_close(L, base);
        int nres = (b != 0 ? b - 1 : cast_int(L->top - ra));
        b = luaD_poscall(L, ci, ra, nres);
        if (ci->callstatus & CIST_FRESH) /* 'ci' still the called one */ {
          /* Lua VM assumes that this case is only
             executed when luaV_execute() is called externally
             i.e. not via OP_CALL, but in JIT mode this is not true
             as luaV_execute() will be called even from OP_CALL when
             a particular function cannot be compiled. So we need
             to somehow trigger the reset of L->top in this case.
             Since luaV_execute is called from various places it is
             more convenient to let the caller decide what to do -
             so we simply return b here. The caller is either OP_CALL
             in JIT mode (see how b is handled in OP_CALL JIT implementation)
             or via luaD_precall() if a JITed function is invoked (see
             ldo.c for how luaD_precall() handles this */
          return b; /* external invocation: return */
        }
        else {  /* invocation via reentry: continue execution */
          ci = L->ci;
          if (b) L->top = ci->top;
          lua_assert(isLua(ci));
          lua_assert(GET_OPCODE(*((ci)->u.l.savedpc - 1)) == OP_CALL);
          goto newframe;  /* restart luaV_execute over new Lua function */
        }
      }
      case OP_FORLOOP: {
        if (ttisinteger(ra)) {  /* integer loop? */
          lua_Integer step = ivalue(ra + 2);
          lua_Integer idx = intop(+, ivalue(ra), step); /* increment index */
          lua_Integer limit = ivalue(ra + 1);
          if ((0 < step) ? (idx <= limit) : (limit <= idx)) {
            ci->u.l.savedpc += GETARG_sBx(i);  /* jump back */
            chgivalue(ra, idx);  /* update internal index... */
            setivalue(ra + 3, idx);  /* ...and external index */
          }
        }
        else {  /* floating loop */
          lua_Number step = fltvalue(ra + 2);
          lua_Number idx = luai_numadd(L, fltvalue(ra), step); /* inc. index */
          lua_Number limit = fltvalue(ra + 1);
          if (luai_numlt(0, step) ? luai_numle(idx, limit)
                                  : luai_numle(limit, idx)) {
            ci->u.l.savedpc += GETARG_sBx(i);  /* jump back */
            chgfltvalue(ra, idx);  /* update internal index... */
            setfltvalue(ra + 3, idx);  /* ...and external index */
          }
        }
      } break;
      case OP_FORPREP: {
        TValue *init = ra;
        TValue *plimit = ra + 1;
        TValue *pstep = ra + 2;
        lua_Integer ilimit;
        int stopnow;
        if (ttisinteger(init) && ttisinteger(pstep) &&
            luaV_forlimit(plimit, &ilimit, ivalue(pstep), &stopnow)) {
          /* all values are integer */
          lua_Integer initv = (stopnow ? 0 : ivalue(init));
          setivalue(plimit, ilimit);
          setivalue(init, intop(-, initv, ivalue(pstep)));
        }
        else {  /* try making all values floats */
          lua_Number ninit; lua_Number nlimit; lua_Number nstep;
          if (!tonumber(plimit, &nlimit))
            luaG_runerror(L, "'for' limit must be a number");
          setfltvalue(plimit, nlimit);
          if (!tonumber(pstep, &nstep))
            luaG_runerror(L, "'for' step must be a number");
          setfltvalue(pstep, nstep);
          if (!tonumber(init, &ninit))
            luaG_runerror(L, "'for' initial value must be a number");
          setfltvalue(init, luai_numsub(L, ninit, nstep));
        }
        ci->u.l.savedpc += GETARG_sBx(i);
      } break;
      case OP_TFORCALL: {
        StkId cb = ra + 3;  /* call base */
        setobjs2s(L, cb+2, ra+2);
        setobjs2s(L, cb+1, ra+1);
        setobjs2s(L, cb, ra);
        L->top = cb + 3;  /* func. + 2 args (state and index) */
        Protect(luaD_call(L, cb, GETARG_C(i)));
        L->top = ci->top;
        i = *(ci->u.l.savedpc++);  /* go to next instruction */
        ra = RA(i);
        lua_assert(GET_OPCODE(i) == OP_TFORLOOP);
        goto l_tforloop;
      }
      case OP_TFORLOOP: {
        l_tforloop:
        if (!ttisnil(ra + 1)) {  /* continue loop? */
          setobjs2s(L, ra, ra + 1);  /* save control variable */
          ci->u.l.savedpc += GETARG_sBx(i);  /* jump back */
        }
      } break;
      case OP_SETLIST: {
        int n = GETARG_B(i);
        int c = GETARG_C(i);
        unsigned int last;
        Table *h;
        if (n == 0) n = cast_int(L->top - ra) - 1;
        if (c == 0) {
          lua_assert(GET_OPCODE(*ci->u.l.savedpc) == OP_EXTRAARG);
          c = GETARG_Ax(*ci->u.l.savedpc++);
        }
        h = hvalue(ra);
        last = ((c-1)*LFIELDS_PER_FLUSH) + n;
        if (h->ravi_array.array_type == RAVI_TTABLE) {
          if (last > h->sizearray)  /* needs more space? */
            luaH_resizearray(L, h, last);  /* pre-allocate it at once */
          for (; n > 0; n--) {
            TValue *val = ra + n;
            luaH_setint(L, h, last--, val);
            luaC_barrierback(L, h, val);
          }
        }
        else {
          int i = last - n + 1;
          for (; i <= (int)last; i++) {
            TValue *val = ra + i;
            unsigned int u = (unsigned int)(i);
            switch (h->ravi_array.array_type) {
              case RAVI_TARRAYINT: {
                if (ttisinteger(val)) {
                  raviH_set_int_inline(L, h, u, ivalue(val));
                }
                else {
                  lua_Integer i = 0;
                  if (luaV_tointeger_(val, &i)) {
                    raviH_set_int_inline(L, h, u, i);
                  }
                  else
                    luaG_runerror(L, "value cannot be converted to integer");
                }
              } break;
              case RAVI_TARRAYFLT: {
                if (ttisfloat(val)) {
                  raviH_set_float_inline(L, h, u, fltvalue(val));
                }
                else if (ttisinteger(val)) {
                  raviH_set_float_inline(L, h, u, (lua_Number)(ivalue(val)));
                }
                else {
                  lua_Number d = 0.0;
                  if (luaV_tonumber_(val, &d)) {
                    raviH_set_float_inline(L, h, u, d);
                  }
                  else
                    luaG_runerror(L, "value cannot be converted to number");
                }
              } break;
              default: lua_assert(0);
            }
          }
        }
        L->top = ci->top; /* correct top (in case of previous open call) */
      } break;
      case OP_CLOSURE: {
        Proto *p = cl->p->p[GETARG_Bx(i)];
        LClosure *ncl = getcached(p, cl->upvals, base);  /* cached closure */
        if (ncl == NULL)  /* no match? */
          pushclosure(L, p, cl->upvals, base, ra);  /* create a new one */
        else
          setclLvalue(L, ra, ncl);  /* push cashed closure */
        checkGC(L, ra + 1);
      } break;
      case OP_VARARG: {
        int b = GETARG_B(i) - 1;  /* required results */
        int j;
        int n = cast_int(base - ci->func) - cl->p->numparams - 1;
        if (n < 0)  /* less arguments than parameters? */
          n = 0;  /* no vararg arguments */
        if (b < 0) {  /* B == 0? */
          b = n;  /* get all var. arguments */
          Protect(luaD_checkstack(L, n));
          ra = RA(i);  /* previous call may change the stack */
          L->top = ra + n;
        }
        for (j = 0; j < b && j < n; j++)
          setobjs2s(L, ra + j, base - n + j);
        for (; j < b; j++)  /* complete required results with nil */
          setnilvalue(ra + j);
      } break;
      case OP_EXTRAARG: {
        lua_assert(0);
      } break;

      case OP_RAVI_BAND_II: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        setivalue(ra, intop(&, ivalue(rb), ivalue(rc)));
      } break;
      case OP_RAVI_BOR_II: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        setivalue(ra, intop(| , ivalue(rb), ivalue(rc)));
      } break;
      case OP_RAVI_BXOR_II: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        setivalue(ra, intop(^, ivalue(rb), ivalue(rc)));
      } break;
      case OP_RAVI_SHL_II: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        setivalue(ra, luaV_shiftl(ivalue(rb), ivalue(rc)));
      } break;
      case OP_RAVI_SHR_II: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Integer ic = ivalue(rc);
        setivalue(ra, luaV_shiftl(ivalue(rb), -ic));
      } break;
      case OP_RAVI_BNOT_I: {
        /* On Win32 the following code generates a test failure
        * at line 29 of bitwise.lua test. Specifically following fails:
        * function x()
        *   local a= 0xF0000000
        *   local b=~a
        *   local c=~b
        *   local d=~~a
        *   print(a,b,c,d)
        *   print(~~a)
        * end
        * Inserting a prinf statement following the assignment to ib appears
        * to cause the problem to go away so this is a case of incorrect
        * optimization / code generation?
        * To work around this issue, for now we can disable the type
        * inference for OP_BNOT in line 1129 of lcode.c (codeexpval function)
        */
        TValue *rb = RB(i);
        lua_Integer ib = ivalue(rb);
        setivalue(ra, intop(^, ~l_castS2U(0), ib));
      } break;

      case OP_RAVI_EQ_II: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        int equals = (ivalue(rb) == ivalue(rc));
        if (equals != GETARG_A(i))
          ci->u.l.savedpc++;
        else
          donextjump(ci);
      } break;
      case OP_RAVI_EQ_FF: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        int equals = (fltvalue(rb) == fltvalue(rc));
        if (equals != GETARG_A(i))
          ci->u.l.savedpc++;
        else
          donextjump(ci);
      } break;
      case OP_RAVI_LT_II: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        int lessthan = (ivalue(rb) < ivalue(rc));
        if (lessthan != GETARG_A(i))
          ci->u.l.savedpc++;
        else
          donextjump(ci);
      } break;
      case OP_RAVI_LT_FF: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        int lessthan = (fltvalue(rb) < fltvalue(rc));
        if (lessthan != GETARG_A(i))
          ci->u.l.savedpc++;
        else
          donextjump(ci);
      } break;
      case OP_RAVI_LE_II: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        int lessequals = (ivalue(rb) <= ivalue(rc));
        if (lessequals != GETARG_A(i))
          ci->u.l.savedpc++;
        else
          donextjump(ci);
      } break;
      case OP_RAVI_LE_FF: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        int lessequals = (fltvalue(rb) <= fltvalue(rc));
        if (lessequals != GETARG_A(i))
          ci->u.l.savedpc++;
        else
          donextjump(ci);
      } break;

      case OP_RAVI_FORLOOP_IP:
      case OP_RAVI_FORLOOP_I1: {
        lua_Integer step = op == OP_RAVI_FORLOOP_I1 ? 1 : ivalue(ra + 2);
        lua_Integer idx = ivalue(ra) + step; /* increment index */
        lua_Integer limit = ivalue(ra + 1);
        if (idx <= limit) {
          ci->u.l.savedpc += GETARG_sBx(i); /* jump back */
          chgivalue(ra, idx);               /* update internal index... */
          setivalue(ra + 3, idx);           /* ...and external index */
        }
      } break;
      case OP_RAVI_FORPREP_IP:
      case OP_RAVI_FORPREP_I1: {
        TValue *pinit = ra;
        TValue *plimit = ra + 1;
        TValue *pstep = (op == OP_RAVI_FORPREP_I1) ? NULL : ra + 2;
        lua_Integer ilimit = ivalue(plimit);
        lua_Integer initv = ivalue(pinit);
        lua_Integer istep = (op == OP_RAVI_FORPREP_I1) ? 1 : ivalue(pstep);
        setivalue(plimit, ilimit);
        setivalue(pinit, initv - istep);
        ci->u.l.savedpc += GETARG_sBx(i);
      } break;

      case OP_RAVI_NEWARRAYI: {
        Table *t = raviH_new(L, RAVI_TARRAYINT);
        sethvalue(L, ra, t);
        checkGC(L, ra + 1);
      } break;
      case OP_RAVI_NEWARRAYF: {
        Table *t = raviH_new(L, RAVI_TARRAYFLT);
        sethvalue(L, ra, t);
        checkGC(L, ra + 1);
      } break;

      case OP_RAVI_GETTABLE_I: {
        TValue *rb = RB(i);
        TValue *rc = RKC(i);
        lua_Integer idx = ivalue(rc);
        Table *t = hvalue(rb);
        const TValue *v;
        if (l_castS2U(idx - 1) < t->sizearray)
          v = &t->array[idx - 1];
        else
          v = luaH_getint(t, idx);
        if (!ttisnil(v) || metamethod_absent(t->metatable, TM_INDEX)) {
          setobj2s(L, ra, v);
        }
        else {
          Protect(raviV_finishget(L, rb, rc, ra));
        }
      } break;
      case OP_RAVI_SELF_S:
      case OP_RAVI_GETTABLE_S: {
        /* Following is an inline version of luaH_getstr() - this is
        * not ideal as there is code duplication; should be changed to a common
        * macro which can be used in bothe places
        */
        StkId rb = RB(i);
        if (op == OP_RAVI_SELF_S) { setobjs2s(L, ra + 1, rb); }
        {
          TValue *rc = k + INDEXK(GETARG_C(i));
          TString *key = tsvalue(rc);
          lua_assert(key->tt == LUA_TSHRSTR);
          Table *h = hvalue(rb);
          int position = lmod(key->hash, sizenode(h));
          Node *n = &h->node[position];
          const TValue *v;
          for (;;) { /* check whether 'key' is somewhere in the chain */
            const TValue *k = gkey(n);
            if (ttisshrstring(k) && eqshrstr(tsvalue(k), key)) {
              v = gval(n); /* that's it */
              break;
            }
            else {
              int nx = gnext(n);
              if (nx == 0) {
                v = luaO_nilobject;
                break;
              }
              n += nx;
            }
          }
          if (!ttisnil(v) || metamethod_absent(h->metatable, TM_INDEX)) {
            setobj2s(L, ra, v);
          }
          else {
            Protect(raviV_finishget(L, rb, rc, ra));
          }
        }
      } break;
      case OP_RAVI_GETTABLE_AI: {
        TValue *rb = RB(i);
        TValue *rc = RKC(i);
        lua_Integer idx = ivalue(rc);
        Table *t = hvalue(rb);
        raviH_get_int_inline(L, t, idx, ra);
      } break;
      case OP_RAVI_GETTABLE_AF: {
        TValue *rb = RB(i);
        TValue *rc = RKC(i);
        lua_Integer idx = ivalue(rc);
        Table *t = hvalue(rb);
        raviH_get_float_inline(L, t, idx, ra);
      } break;
      case OP_RAVI_SETTABLE_AI: {
        Table *t = hvalue(ra);
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Integer idx = ivalue(rb);
        if (ttisinteger(rc)) { raviH_set_int_inline(L, t, idx, ivalue(rc)); }
        else if (ttisfloat(rc)) {
          raviH_set_int_inline(L, t, idx, (lua_Integer)fltvalue(rc));
        }
        else {
          lua_Integer j;
          if (tointeger(rc, &j)) { raviH_set_int_inline(L, t, idx, j); }
          else
            luaG_runerror(L, "integer expected");
        }
      } break;
      case OP_RAVI_SETTABLE_AII: {
        Table *t = hvalue(ra);
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Integer idx = ivalue(rb);
        raviH_set_int_inline(L, t, idx, ivalue(rc));
      } break;
      case OP_RAVI_SETTABLE_AF: {
        Table *t = hvalue(ra);
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Integer idx = ivalue(rb);
        if (ttisfloat(rc)) { raviH_set_float_inline(L, t, idx, fltvalue(rc)); }
        else if (ttisinteger(rc)) {
          raviH_set_float_inline(L, t, idx, ((lua_Number)ivalue(rc)));
        }
        else {
          lua_Number j;
          if (tonumber(rc, &j)) { raviH_set_float_inline(L, t, idx, j); }
          else
            luaG_runerror(L, "number expected");
        }
      } break;
      case OP_RAVI_SETTABLE_AFF: {
        Table *t = hvalue(ra);
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        lua_Integer idx = ivalue(rb);
        raviH_set_float_inline(L, t, idx, fltvalue(rc));
      } break;

      case OP_RAVI_SETUPVALI: {
        lua_Integer ia;
        if (tointeger(ra, &ia)) {
          UpVal *uv = cl->upvals[GETARG_B(i)];
          setivalue(uv->v, ia);
          luaC_upvalbarrier(L, uv);
        }
        else
          luaG_runerror(
            L, "upvalue of integer type, cannot be set to non integer value");
      } break;
      case OP_RAVI_SETUPVALF: {
        lua_Number na;
        if (tonumber(ra, &na)) {
          UpVal *uv = cl->upvals[GETARG_B(i)];
          setfltvalue(uv->v, na);
          luaC_upvalbarrier(L, uv);
        }
        else
          luaG_runerror(
            L, "upvalue of number type, cannot be set to non number value");
      } break;

      case OP_RAVI_SETUPVALAI: {
        if (!ttistable(ra) ||
          hvalue(ra)->ravi_array.array_type != RAVI_TARRAYINT)
          luaG_runerror(L,
            "upvalue of integer[] type, cannot be set to non "
            "integer[] value");
        UpVal *uv = cl->upvals[GETARG_B(i)];
        setobj(L, uv->v, ra);
        luaC_upvalbarrier(L, uv);
      } break;
      case OP_RAVI_SETUPVALAF: {
        if (!ttistable(ra) ||
          hvalue(ra)->ravi_array.array_type != RAVI_TARRAYFLT)
          luaG_runerror(
            L,
            "upvalue of number[] type, cannot be set to non number[] value");
        UpVal *uv = cl->upvals[GETARG_B(i)];
        setobj(L, uv->v, ra);
        luaC_upvalbarrier(L, uv);
      } break;
      case OP_RAVI_SETUPVALT: {
        if (!ttistable(ra) || hvalue(ra)->ravi_array.array_type != RAVI_TTABLE)
          luaG_runerror(
            L, "upvalue of table type, cannot be set to non table value");
        UpVal *uv = cl->upvals[GETARG_B(i)];
        setobj(L, uv->v, ra);
        luaC_upvalbarrier(L, uv);
      } break;


      case OP_RAVI_LOADIZ: {
        setivalue(ra, 0);
      } break;
      case OP_RAVI_LOADFZ: {
        setfltvalue(ra, 0.0);
      } break;

      case OP_RAVI_UNMF: {
        TValue *rb = RB(i);
        setfltvalue(ra, -fltvalue(rb));
      } break;
      case OP_RAVI_UNMI: {
        TValue *rb = RB(i);
        setivalue(ra, -ivalue(rb));
      } break;

      case OP_RAVI_ADDFF: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        setfltvalue(ra, fltvalue(rb) + fltvalue(rc));
      } break;
      case OP_RAVI_ADDFI: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        setfltvalue(ra, fltvalue(rb) + ivalue(rc));
      } break;
      case OP_RAVI_ADDII: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        setivalue(ra, ivalue(rb) + ivalue(rc));
      } break;

      case OP_RAVI_SUBFF: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        setfltvalue(ra, fltvalue(rb) - fltvalue(rc));
      } break;
      case OP_RAVI_SUBFI: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        setfltvalue(ra, fltvalue(rb) - ivalue(rc));
      } break;
      case OP_RAVI_SUBIF: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        setfltvalue(ra, ivalue(rb) - fltvalue(rc));
      } break;
      case OP_RAVI_SUBII: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        setivalue(ra, ivalue(rb) - ivalue(rc));
      } break;

      case OP_RAVI_MULFF: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        setfltvalue(ra, fltvalue(rb) * fltvalue(rc));
      } break;
      case OP_RAVI_MULFI: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        setfltvalue(ra, fltvalue(rb) * ivalue(rc));
      } break;
      case OP_RAVI_MULII: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        setivalue(ra, ivalue(rb) * ivalue(rc));
      } break;

      case OP_RAVI_DIVFF: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        setfltvalue(ra, fltvalue(rb) / fltvalue(rc));
      } break;
      case OP_RAVI_DIVFI: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        setfltvalue(ra, fltvalue(rb) / ivalue(rc));
      } break;
      case OP_RAVI_DIVIF: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        setfltvalue(ra, ivalue(rb) / fltvalue(rc));
      } break;
      case OP_RAVI_DIVII: {
        TValue *rb = RKB(i);
        TValue *rc = RKC(i);
        setfltvalue(ra, (lua_Number)(ivalue(rb)) / (lua_Number)(ivalue(rc)));
      } break;

      case OP_RAVI_MOVEI: {
        TValue *rb = RB(i);
        lua_Integer j;
        if (tointeger(rb, &j)) { setivalue(ra, j); }
        else
          luaG_runerror(L, "MOVEI: integer expected");
      } break;
      case OP_RAVI_MOVEF: {
        TValue *rb = RB(i);
        lua_Number j;
        if (tonumber(rb, &j)) { setfltvalue(ra, j); }
        else
          luaG_runerror(L, "MOVEF: number expected");
      } break;
      case OP_RAVI_MOVEAI: {
        TValue *rb = RB(i);
        if (ttistable(rb) &&
          hvalue(rb)->ravi_array.array_type == RAVI_TARRAYINT) {
          setobjs2s(L, ra, rb);
        }
        else
          luaG_runerror(L, "integer[] expected");
      } break;
      case OP_RAVI_MOVEAF: {
        TValue *rb = RB(i);
        if (ttistable(rb) &&
          hvalue(rb)->ravi_array.array_type == RAVI_TARRAYFLT) {
          setobjs2s(L, ra, rb);
        }
        else
          luaG_runerror(L, "number[] expected");
      } break;
      case OP_RAVI_MOVETAB: {
        TValue *rb = RB(i);
        if (ttistable(rb) && hvalue(rb)->ravi_array.array_type == RAVI_TTABLE) {
          setobjs2s(L, ra, rb);
        }
        else
          luaG_runerror(L, "table expected");
      } break;

      case OP_RAVI_TOINT: {
        lua_Integer j;
        if (tointeger(ra, &j)) { setivalue(ra, j); }
        else
          luaG_runerror(L, "TOINT: integer expected");
      } break;
      case OP_RAVI_TOFLT: {
        lua_Number j;
        if (tonumber(ra, &j)) { setfltvalue(ra, j); }
        else
          luaG_runerror(L, "TOFLT: number expected");
      } break;
      case OP_RAVI_TOTAB: {
        if (!ttistable(ra) || 
            hvalue(ra)->ravi_array.array_type != RAVI_TTABLE)
          luaG_runerror(L, "table expected");
      } break;
      case OP_RAVI_TOARRAYI: {
        if (!ttistable(ra) ||
            hvalue(ra)->ravi_array.array_type != RAVI_TARRAYINT)
          luaG_runerror(L, "integer[] expected");
      } break;
      case OP_RAVI_TOARRAYF: {
        if (!ttistable(ra) ||
            hvalue(ra)->ravi_array.array_type != RAVI_TARRAYFLT)
          luaG_runerror(L, "number[] expected");
      } break;
    }
  }
}

void ravi_dump_value(lua_State *L, const TValue *stack_ptr) {
  (void)L;
  if (ttisCclosure(stack_ptr))
    printf("C closure\n");
  else if (ttislcf(stack_ptr))
    printf("light C function\n");
  else if (ttisLclosure(stack_ptr))
    printf("Lua closure\n");
  else if (ttisfunction(stack_ptr))
    printf("function\n");
  else if (ttislngstring(stack_ptr) || ttisshrstring(stack_ptr) ||
    ttisstring(stack_ptr))
    printf("'%s'\n", svalue(stack_ptr));
  else if (ttistable(stack_ptr))
    printf("table\n");
  else if (ttisnil(stack_ptr))
    printf("nil\n");
  else if (ttisfloat(stack_ptr))
    printf("%.6f\n", fltvalue(stack_ptr));
  else if (ttisinteger(stack_ptr))
    printf("%lld\n", ivalue(stack_ptr));
  else if (ttislightuserdata(stack_ptr))
    printf("light user data\n");
  else if (ttisfulluserdata(stack_ptr))
    printf("full user data\n");
  else if (ttisboolean(stack_ptr))
    printf("boolean\n");
  else if (ttisthread(stack_ptr))
    printf("thread\n");
  else
    printf("other\n");
}

static void ravi_dump_ci(lua_State *L, CallInfo *ci) {
  StkId func = ci->func;
  int func_type = ttype(func);
  StkId base = NULL;
  Proto *p = NULL;
  int funcpos = ci->func - L->stack;
  StkId stack_ptr = ci->top - 1;
  int i;
  switch (func_type) {
  case LUA_TLCF:
    printf("stack[%d] = Light C function\n", funcpos);
    printf("---> called from \n");
    return;
  case LUA_TCCL:
    printf("stack[%d] = C closure\n", funcpos);
    printf("---> called from \n");
    return;
  case LUA_TFUNCTION:
    p = clLvalue(func)->p;
    base = ci->u.l.base;
    i = ci->top - L->stack - 1;
    break;
  default:
    return;
  }
  for (; stack_ptr >= base; stack_ptr--, i--) {
    printf("stack[%d] reg[%d] = %s %s", i, (int)(stack_ptr-base), (stack_ptr == base ? "(base) " : ""), (stack_ptr == L->top ? "(L->top) " : ""));
    if (ttisCclosure(stack_ptr))
      printf("C closure\n");
    else if (ttislcf(stack_ptr))
      printf("light C function\n");
    else if (ttisLclosure(stack_ptr))
      printf("Lua closure\n");
    else if (ttisfunction(stack_ptr))
      printf("function\n");
    else if (ttislngstring(stack_ptr) || ttisshrstring(stack_ptr) ||
             ttisstring(stack_ptr))
      printf("'%s'\n", svalue(stack_ptr));
    else if (ttistable(stack_ptr))
      printf("table\n");
    else if (ttisnil(stack_ptr))
      printf("nil\n");
    else if (ttisfloat(stack_ptr))
      printf("%.6f\n", fltvalue(stack_ptr));
    else if (ttisinteger(stack_ptr))
      printf("%lld\n", ivalue(stack_ptr));
    else if (ttislightuserdata(stack_ptr))
      printf("light user data\n");
    else if (ttisfulluserdata(stack_ptr))
      printf("full user data\n");
    else if (ttisboolean(stack_ptr))
      printf("boolean\n");
    else if (ttisthread(stack_ptr))
      printf("thread\n");
    else
      printf("other\n");
  }
  printf(
      "stack[%d] = Lua function (registers = %d, params = %d, locals = %d)\n",
      funcpos, (int)(p->maxstacksize), (int)(p->numparams), p->sizelocvars);
  printf("---> called from \n");
}

void ravi_dump_stack(lua_State *L, const char *s) {
  if (!s)
    return;
  CallInfo *ci = L->ci;
  printf("=======================\n");
  printf("Stack dump %s\n", s);
  printf("=======================\n");
  printf("L->top = %d\n", (int)(L->top - L->stack));
  while (ci) {
    ravi_dump_ci(L, ci);
    ci = ci->previous;
  }
  printf("\n");
}

void ravi_dump_stacktop(lua_State *L, const char *s) {
  CallInfo *ci = L->ci;
  int funcpos = (int)(ci->func - L->stack);
  int top = (int)(L->top - L->stack);
  int ci_top = (int)(ci->top - L->stack);
  printf("Stack dump %s function %d L->top = %d, ci->top = %d\n", s, funcpos,
         top, ci_top);
}

/*
** This function is called from JIT compiled code when JIT trace is
** enabled; the function needs to update the savedpc and
** call luaG_traceexec() if necessary
*/
void ravi_debug_trace(lua_State *L, int opCode, int pc) {
  RAVI_DEBUG_STACK(
      char buf[100]; CallInfo *ci = L->ci;
      int funcpos = (int)(ci->func - L->stack);
      int top = (int)(L->top - L->stack);
      int ci_top = (int)(ci->top - L->stack);
      int base = (int)(ci->u.l.base - L->stack); raviP_instruction_to_str(
          buf, sizeof buf, clvalue(L->ci->func)->l.p->code[pc]);
      printf(
          "Stack dump %s (%s) function %d, pc=%d, L->top = %d, ci->top = %d\n",
          luaP_opnames[opCode], buf, funcpos, pc, (top - base),
          (ci_top - base));
      lua_assert(L->ci->u.l.base <= L->top &&
                 L->top < L->stack + L->stacksize);)

  // Updates the savedpc pointer in the call frame
  // The savedpc is unimportant for the JIT but it is relied upon
  // by the debug interface. So we need to set this in order for the
  // debug api to work. Rather than setting it on every bytecode instruction
  // we have a dual approach. By default the JIT code only sets this prior to
  // function calls - this enables better stack traces for example, and ad-hoc
  // calls to debug api.
  // See void RaviCodeGenerator::emit_update_savedpc(RaviFunctionDef * def,
  // int pc) which is used for this purpose.
  // An optional setting in the JIT compiler also
  // enables this to be updated per bytecode instruction - this is only
  // required if someone wishes to set a line hook. The second option
  // is very expensive and will inhibit optimizations, hence it is optional.
  // This is the setting that is done below - and then the hook is invoked
  // See issue #15
  LClosure *closure = clLvalue(L->ci->func);
  L->ci->u.l.savedpc = &closure->p->code[pc + 1];
  if (L->hookmask & (LUA_MASKLINE | LUA_MASKCOUNT)) luaG_traceexec(L);
}

void raviV_op_newarrayint(lua_State *L, CallInfo *ci, TValue *ra) {
  Table *t = raviH_new(L, RAVI_TARRAYINT);
  sethvalue(L, ra, t);
  checkGC_(L, ra + 1);
}

void raviV_op_newarrayfloat(lua_State *L, CallInfo *ci, TValue *ra) {
  Table *t = raviH_new(L, RAVI_TARRAYFLT);
  sethvalue(L, ra, t);
  checkGC_(L, ra + 1);
}

void raviV_op_newtable(lua_State *L, CallInfo *ci, TValue *ra, int b, int c) {
  Table *t = luaH_new(L);
  sethvalue(L, ra, t);
  if (b != 0 || c != 0) luaH_resize(L, t, luaO_fb2int(b), luaO_fb2int(c));
  checkGC_(L, ra + 1);
}

void raviV_op_setlist(lua_State *L, CallInfo *ci, TValue *ra, int b, int c) {
  int n = b;
  unsigned int last;
  Table *h;
  if (n == 0) n = cast_int(L->top - ra) - 1;
  h = hvalue(ra);
  last = ((c - 1) * LFIELDS_PER_FLUSH) + n;
  if (h->ravi_array.array_type == RAVI_TTABLE) {
    if (last > h->sizearray)        /* needs more space? */
      luaH_resizearray(L, h, last); /* pre-allocate it at once */
    for (; n > 0; n--) {
      TValue *val = ra + n;
      luaH_setint(L, h, last--, val);
      luaC_barrierback(L, h, val);
    }
  }
  else {
    int i = last - n + 1;
    for (; i <= (int)last; i++) {
      TValue *val = ra + i;
      unsigned int u = (unsigned int)(i);
      switch (h->ravi_array.array_type) {
        case RAVI_TARRAYINT: {
          if (ttisinteger(val)) { raviH_set_int_inline(L, h, u, ivalue(val)); }
          else {
            lua_Integer i = 0;
            if (luaV_tointeger_(val, &i)) { raviH_set_int_inline(L, h, u, i); }
            else
              luaG_runerror(L, "value cannot be converted to integer");
          }
        } break;
        case RAVI_TARRAYFLT: {
          if (ttisfloat(val)) {
            raviH_set_float_inline(L, h, u, fltvalue(val));
          }
          else if (ttisinteger(val)) {
            raviH_set_float_inline(L, h, u, (lua_Number)(ivalue(val)));
          }
          else {
            lua_Number d = 0.0;
            if (luaV_tonumber_(val, &d)) { raviH_set_float_inline(L, h, u, d); }
            else
              luaG_runerror(L, "value cannot be converted to number");
          }
        } break;
        default: lua_assert(0);
      }
    }
  }
  L->top = ci->top; /* correct top (in case of previous open call) */
}

void raviV_op_concat(lua_State *L, CallInfo *ci, int a, int b, int c) {
  StkId rb, ra;
  StkId base = ci->u.l.base;
  L->top = base + c + 1; /* mark the end of concat operands */
  Protect(luaV_concat(L, c - b + 1));
  ra = base + a; /* 'luav_concat' may invoke TMs and move the stack */
  rb = base + b;
  setobjs2s(L, ra, rb);
  checkGC(L, (ra >= rb ? ra + 1 : rb));
  L->top = ci->top; /* restore top */
}

void raviV_op_closure(lua_State *L, CallInfo *ci, LClosure *cl, int a, int Bx) {
  StkId base = ci->u.l.base;
  Proto *p = cl->p->p[Bx];
  LClosure *ncl = getcached(p, cl->upvals, base); /* cached closure */
  StkId ra = base + a;
  if (ncl == NULL) /* no match? */ {
    pushclosure(L, p, cl->upvals, base, ra); /* create a new one */
  }
  else {
    setclLvalue(L, ra, ncl); /* push cashed closure */
  }
  checkGC(L, ra + 1);
}

void raviV_op_vararg(lua_State *L, CallInfo *ci, LClosure *cl, int a, int b) {
  StkId base = ci->u.l.base;
  StkId ra;
  int j;
  int n = cast_int(base - ci->func) - cl->p->numparams - 1;
  if (n < 0) /* less arguments than parameters? */
    n = 0;   /* no vararg arguments */
  b = b - 1;
  if (b < 0) { /* B == 0? */
    b = n;     /* get all var. arguments */
    Protect(luaD_checkstack(L, n));
    ra = base + a; /* previous call may change the stack */
    L->top = ra + n;
  }
  else {
    ra = base + a;
  }
  for (j = 0; j < b && j < n; j++) 
    setobjs2s(L, ra + j, base - n + j);
  for (; j < b; j++) /* complete required results with nil */
    setnilvalue(ra + j);
}

// This is a cheat for a boring opcode
void raviV_op_loadnil(CallInfo *ci, int a, int b) {
  StkId base;
  base = ci->u.l.base;
  TValue *ra = base + a;
  do { setnilvalue(ra++); } while (b--);
}

void raviV_op_setupvali(lua_State *L, LClosure *cl, TValue *ra, int b) {
  lua_Integer ia;
  if (tointeger(ra, &ia)) {
    UpVal *uv = cl->upvals[b];
    setivalue(uv->v, ia);
    luaC_upvalbarrier(L, uv);
  }
  else
    luaG_runerror(
        L, "upvalue of integer type, cannot be set to non integer value");
}

void raviV_op_setupvalf(lua_State *L, LClosure *cl, TValue *ra, int b) {
  lua_Number na;
  if (tonumber(ra, &na)) {
    UpVal *uv = cl->upvals[b];
    setfltvalue(uv->v, na);
    luaC_upvalbarrier(L, uv);
  }
  else
    luaG_runerror(L,
                  "upvalue of number type, cannot be set to non number value");
}

void raviV_op_setupvalai(lua_State *L, LClosure *cl, TValue *ra, int b) {
  if (!ttistable(ra) || hvalue(ra)->ravi_array.array_type != RAVI_TARRAYINT)
    luaG_runerror(
        L, "upvalue of integer[] type, cannot be set to non integer[] value");
  UpVal *uv = cl->upvals[b];
  setobj(L, uv->v, ra);
  luaC_upvalbarrier(L, uv);
}

void raviV_op_setupvalaf(lua_State *L, LClosure *cl, TValue *ra, int b) {
  if (!ttistable(ra) || hvalue(ra)->ravi_array.array_type != RAVI_TARRAYFLT)
    luaG_runerror(
        L, "upvalue of number[] type, cannot be set to non number[] value");
  UpVal *uv = cl->upvals[b];
  setobj(L, uv->v, ra);
  luaC_upvalbarrier(L, uv);
}

void raviV_op_setupvalt(lua_State *L, LClosure *cl, TValue *ra, int b) {
  if (!ttistable(ra) || hvalue(ra)->ravi_array.array_type != RAVI_TTABLE)
    luaG_runerror(L, "upvalue of table type, cannot be set to non table value");
  UpVal *uv = cl->upvals[b];
  setobj(L, uv->v, ra);
  luaC_upvalbarrier(L, uv);
}

void raviV_op_setupval(lua_State *L, LClosure *cl, TValue *ra, int b) {
  UpVal *uv = cl->upvals[b];
  setobj(L, uv->v, ra);
  luaC_upvalbarrier(L, uv);
}

void raviV_op_shl(lua_State *L, TValue *ra, TValue *rb, TValue *rc) {
  lua_Integer ib;
  lua_Integer ic;
  if (tointeger(rb, &ib) && tointeger(rc, &ic)) {
    setivalue(ra, luaV_shiftl(ib, ic));
  }
  else {
    luaT_trybinTM(L, rb, rc, ra, TM_SHL);
  }
}

void raviV_op_shr(lua_State *L, TValue *ra, TValue *rb, TValue *rc) {
  lua_Integer ib;
  lua_Integer ic;
  if (tointeger(rb, &ib) && tointeger(rc, &ic)) {
    setivalue(ra, luaV_shiftl(ib, -ic));
  }
  else {
    luaT_trybinTM(L, rb, rc, ra, TM_SHR);
  }
}

void raviV_finishget(lua_State *L, const TValue *t, TValue *key, StkId val) {
  int loop; /* counter to avoid infinite loops */
  const TValue *tm = luaT_gettm(hvalue(t), TM_INDEX, G(L)->tmname[TM_INDEX]);
  for (loop = 0; loop < MAXTAGLOOP; loop++) {
    if (tm == NULL) { /* no metamethod (from a table)? */
      if (ttisnil(tm = luaT_gettmbyobj(L, t, TM_INDEX)))
        luaG_typeerror(L, t, "index"); /* no metamethod */
    }
    if (ttisfunction(tm)) {               /* metamethod is a function */
      luaT_callTM(L, tm, t, key, val, 1); /* call it */
      return;
    }
    t = tm; /* else repeat access over 'tm' */
    if (luaV_fastget(L, t, key, tm, luaH_get)) { /* try fast track */
      setobj2s(L, val, tm);                      /* done */
      return;
    }
    /* else repeat */
  }
  luaG_runerror(L, "gettable chain too long; possible loop");
}

/* }================================================================== */

