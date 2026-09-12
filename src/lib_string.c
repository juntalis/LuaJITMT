/*
** String library.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lib_string_c
#define LUA_LIB

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_atomic.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_ff.h"
#include "lj_bcdump.h"
#include "lj_char.h"
#include "lj_strfmt.h"
#include "lj_lib.h"

/* ------------------------------------------------------------------------ */

#define LJLIB_MODULE_string

LJLIB_LUA(string_len) /*
  function(s)
    CHECK_str(s)
    return #s
  end
*/

LJLIB_ASM(string_byte)		LJLIB_REC(string_range 0)
{
  GCstr *s = lj_lib_checkstr(L, 1);
  int32_t len = (int32_t)s->len;
  int32_t start = lj_lib_optint(L, 2, 1);
  int32_t stop = lj_lib_optint(L, 3, start);
  int32_t n, i;
  const unsigned char *p;
  if (stop < 0) stop += len+1;
  if (start < 0) start += len+1;
  if (start <= 0) start = 1;
  if (stop > len) stop = len;
  if (start > stop) return FFH_RES(0);  /* Empty interval: return no results. */
  start--;
  n = stop - start;
  if ((uint32_t)n > LUAI_MAXCSTACK)
    lj_err_caller(L, LJ_ERR_STRSLC);
  lj_state_checkstack(L, (MSize)n);
  p = (const unsigned char *)strdata(s) + start;
  for (i = 0; i < n; i++)
    setintV(L->base + i-1-LJ_FR2, p[i]);
  return FFH_RES(n);
}

LJLIB_ASM(string_char)		LJLIB_REC(.)
{
  int i, nargs = (int)(L->top - L->base);
  char *buf = lj_buf_tmp(L, (MSize)nargs);
  for (i = 1; i <= nargs; i++) {
    int32_t k = lj_lib_checkint(L, i);
    if (!checku8(k))
      lj_err_arg(L, i, LJ_ERR_BADVAL);
    buf[i-1] = (char)k;
  }
  setstrV(L, L->base-1-LJ_FR2, lj_str_new(L, buf, (size_t)nargs));
  return FFH_RES(1);
}

LJLIB_ASM(string_sub)		LJLIB_REC(string_range 1)
{
  lj_lib_checkstr(L, 1);
  lj_lib_checkint(L, 2);
  setintV(L->base+2, lj_lib_optint(L, 3, -1));
  return FFH_RETRY;
}

LJLIB_CF(string_rep)		LJLIB_REC(.)
{
  GCstr *s = lj_lib_checkstr(L, 1);
  int32_t rep = lj_lib_checkint(L, 2);
  GCstr *sep = lj_lib_optstr(L, 3);
  SBuf *sb = lj_buf_tmp_(L);
  if (sep && rep > 1) {
    GCstr *s2 = lj_buf_cat2str(L, sep, s);
    lj_buf_reset(sb);
    lj_buf_putstr(sb, s);
    s = s2;
    rep--;
  }
  sb = lj_buf_putstr_rep(sb, s, rep);
  setstrV(L, L->top-1, lj_buf_str(L, sb));
  lj_gc_pubroot(L, L->top-1);
  lj_gc_check(L);
  return 1;
}

LJLIB_ASM(string_reverse)  LJLIB_REC(string_op IRCALL_lj_buf_putstr_reverse)
{
  lj_lib_checkstr(L, 1);
  return FFH_RETRY;
}
LJLIB_ASM_(string_lower)  LJLIB_REC(string_op IRCALL_lj_buf_putstr_lower)
LJLIB_ASM_(string_upper)  LJLIB_REC(string_op IRCALL_lj_buf_putstr_upper)

/* ------------------------------------------------------------------------ */

static int writer_buf(lua_State *L, const void *p, size_t size, void *sb)
{
  lj_buf_putmem((SBuf *)sb, p, (MSize)size);
  UNUSED(L);
  return 0;
}

LJLIB_CF(string_dump)
{
  GCproto *pt = lj_lib_checkLproto(L, 1, 1);
  uint32_t flags = 0;
  SBuf *sb;
  TValue *o = L->base+1;
  if (o < L->top) {
    if (tvisstr(o)) {
      const char *mode = strVdata(o);
      char c;
      while ((c = *mode++)) {
	if (c == 's') flags |= BCDUMP_F_STRIP;
	if (c == 'd') flags |= BCDUMP_F_DETERMINISTIC;
      }
    } else if (tvistruecond(o)) {
      flags |= BCDUMP_F_STRIP;
    }
  }
  sb = lj_buf_tmp_(L);  /* Assumes lj_bcwrite() doesn't use tmpbuf. */
  L->top = L->base+1;
  if (!pt || lj_bcwrite(L, pt, writer_buf, sb, flags))
    lj_err_caller(L, LJ_ERR_STRDUMP);
  setstrV(L, L->top-1, lj_buf_str(L, sb));
  lj_gc_check(L);
  return 1;
}

/* ------------------------------------------------------------------------ */

/* macro to `unsign' a character */
#define uchar(c)	((unsigned char)(c))

#define CAP_UNFINISHED	(-1)
#define CAP_POSITION	(-2)

typedef struct MatchState {
  const char *src_init;  /* init of source string */
  const char *src_end;  /* end of source string */
  const char *p_end;  /* end of pattern string */
  lua_State *L;
  int level;  /* total number of captures (finished or unfinished) */
  int depth;
  struct {
    const char *init;
    ptrdiff_t len;
  } capture[LUA_MAXCAPTURES];
} MatchState;

#define L_ESC		'%'

static int check_capture(MatchState *ms, int l)
{
  l -= '1';
  if (l < 0 || l >= ms->level || ms->capture[l].len == CAP_UNFINISHED)
    lj_err_caller(ms->L, LJ_ERR_STRCAPI);
  return l;
}

static int capture_to_close(MatchState *ms)
{
  int level = ms->level;
  for (level--; level>=0; level--)
    if (ms->capture[level].len == CAP_UNFINISHED) return level;
  lj_err_caller(ms->L, LJ_ERR_STRPATC);
  return 0;  /* unreachable */
}

static const char *classend(MatchState *ms, const char *p)
{
  lj_assertX(p < ms->p_end, "pattern read past end");
  switch (*p++) {
  case L_ESC:
    if (p >= ms->p_end)
      lj_err_caller(ms->L, LJ_ERR_STRPATE);
    return p+1;
  case '[':
    if (p < ms->p_end && *p == '^') p++;
    do {  /* look for a `]' */
      if (p >= ms->p_end)
	lj_err_caller(ms->L, LJ_ERR_STRPATM);
      if (*(p++) == L_ESC && p < ms->p_end)
	p++;  /* skip escapes (e.g. `%]') */
    } while (p >= ms->p_end || *p != ']');
    return p+1;
  default:
    return p;
  }
}

static const unsigned char match_class_map[32] = {
  0,LJ_CHAR_ALPHA,0,LJ_CHAR_CNTRL,LJ_CHAR_DIGIT,0,0,LJ_CHAR_GRAPH,0,0,0,0,
  LJ_CHAR_LOWER,0,0,0,LJ_CHAR_PUNCT,0,0,LJ_CHAR_SPACE,0,
  LJ_CHAR_UPPER,0,LJ_CHAR_ALNUM,LJ_CHAR_XDIGIT,0,0,0,0,0,0,0
};

static int match_class(int c, int cl)
{
  if ((cl & 0xc0) == 0x40) {
    int t = match_class_map[(cl&0x1f)];
    if (t) {
      t = lj_char_isa(c, t);
      return (cl & 0x20) ? t : !t;
    }
    if (cl == 'z') return c == 0;
    if (cl == 'Z') return c != 0;
  }
  return (cl == c);
}

static int matchbracketclass(int c, const char *p, const char *ec)
{
  int sig = 1;
  if (*(p+1) == '^') {
    sig = 0;
    p++;  /* skip the `^' */
  }
  while (++p < ec) {
    if (*p == L_ESC) {
      p++;
      if (match_class(c, uchar(*p)))
	return sig;
    }
    else if ((*(p+1) == '-') && (p+2 < ec)) {
      p+=2;
      if (uchar(*(p-2)) <= c && c <= uchar(*p))
	return sig;
    }
    else if (uchar(*p) == c) return sig;
  }
  return !sig;
}

static int singlematch(MatchState *ms, const char *s, const char *p,
		       const char *ep)
{
  int c;
  if (s >= ms->src_end)
    return 0;
  c = uchar(*s);
  switch (*p) {
  case '.': return 1;  /* matches any char */
  case L_ESC: return match_class(c, uchar(*(p+1)));
  case '[': return matchbracketclass(c, p, ep-1);
  default:  return (uchar(*p) == c);
  }
}

static const char *match(MatchState *ms, const char *s, const char *p);

static const char *matchbalance(MatchState *ms, const char *s, const char *p)
{
  if ((size_t)(ms->p_end - p) < 2)
    lj_err_caller(ms->L, LJ_ERR_STRPATU);
  if (s >= ms->src_end || *s != *p) {
    return NULL;
  } else {
    int b = *p;
    int e = *(p+1);
    int cont = 1;
    while (++s < ms->src_end) {
      if (*s == e) {
	if (--cont == 0) return s+1;
      } else if (*s == b) {
	cont++;
      }
    }
  }
  return NULL;  /* string ends out of balance */
}

static const char *max_expand(MatchState *ms, const char *s,
			      const char *p, const char *ep)
{
  ptrdiff_t i = 0;  /* counts maximum expand for item */
  while (singlematch(ms, s+i, p, ep))
    i++;
  /* keeps trying to match with the maximum repetitions */
  while (i>=0) {
    const char *res = match(ms, (s+i), ep+1);
    if (res) return res;
    i--;  /* else didn't match; reduce 1 repetition to try again */
  }
  return NULL;
}

static const char *min_expand(MatchState *ms, const char *s,
			      const char *p, const char *ep)
{
  for (;;) {
    const char *res = match(ms, s, ep+1);
    if (res != NULL)
      return res;
    else if (singlematch(ms, s, p, ep))
      s++;  /* try with one more repetition */
    else
      return NULL;
  }
}

static const char *start_capture(MatchState *ms, const char *s,
				 const char *p, int what)
{
  const char *res;
  int level = ms->level;
  if (level >= LUA_MAXCAPTURES) lj_err_caller(ms->L, LJ_ERR_STRCAPN);
  ms->capture[level].init = s;
  ms->capture[level].len = what;
  ms->level = level+1;
  if ((res=match(ms, s, p)) == NULL)  /* match failed? */
    ms->level--;  /* undo capture */
  return res;
}

static const char *end_capture(MatchState *ms, const char *s,
			       const char *p)
{
  int l = capture_to_close(ms);
  const char *res;
  ms->capture[l].len = s - ms->capture[l].init;  /* close capture */
  if ((res = match(ms, s, p)) == NULL)  /* match failed? */
    ms->capture[l].len = CAP_UNFINISHED;  /* undo capture */
  return res;
}

static const char *match_capture(MatchState *ms, const char *s, int l)
{
  size_t len;
  l = check_capture(ms, l);
  len = (size_t)ms->capture[l].len;
  if ((size_t)(ms->src_end-s) >= len &&
      memcmp(ms->capture[l].init, s, len) == 0)
    return s+len;
  else
    return NULL;
}

static const char *match(MatchState *ms, const char *s, const char *p)
{
  if (++ms->depth > LJ_MAX_XLEVEL)
    lj_err_caller(ms->L, LJ_ERR_STRPATX);
  init: /* using goto's to optimize tail recursion */
  if (p == ms->p_end) {  /* A NUL before p_end is an ordinary literal. */
    ms->depth--;
    return s;
  }
  switch (*p) {
  case '(':  /* start capture */
    if (p+1 < ms->p_end && *(p+1) == ')')  /* position capture? */
      s = start_capture(ms, s, p+2, CAP_POSITION);
    else
      s = start_capture(ms, s, p+1, CAP_UNFINISHED);
    break;
  case ')':  /* end capture */
    s = end_capture(ms, s, p+1);
    break;
  case L_ESC:
    if (p+1 >= ms->p_end)
      lj_err_caller(ms->L, LJ_ERR_STRPATE);
    switch (*(p+1)) {
    case 'b':  /* balanced string? */
      s = matchbalance(ms, s, p+2);
      if (s == NULL) break;
      p+=4;
      goto init;  /* else s = match(ms, s, p+4); */
    case 'f': {  /* frontier? */
      const char *ep; char previous;
      p += 2;
      if (p >= ms->p_end || *p != '[')
	lj_err_caller(ms->L, LJ_ERR_STRPATB);
      ep = classend(ms, p);  /* points to what is next */
      previous = (s == ms->src_init) ? '\0' : *(s-1);
      if (matchbracketclass(uchar(previous), p, ep-1) ||
	 !matchbracketclass(s < ms->src_end ? uchar(*s) : 0,
			    p, ep-1)) { s = NULL; break; }
      p=ep;
      goto init;  /* else s = match(ms, s, ep); */
      }
    default:
      if (lj_char_isdigit(uchar(*(p+1)))) {  /* capture results (%0-%9)? */
	s = match_capture(ms, s, uchar(*(p+1)));
	if (s == NULL) break;
	p+=2;
	goto init;  /* else s = match(ms, s, p+2) */
      }
      goto dflt;  /* case default */
    }
    break;
  case '$':
    /* is the `$' the last char in pattern? */
    if (p+1 != ms->p_end) goto dflt;
    if (s != ms->src_end) s = NULL;  /* check end of string */
    break;
  default: dflt: {  /* it is a pattern item */
    const char *ep = classend(ms, p);  /* points to what is next */
    int m = singlematch(ms, s, p, ep);
    int suffix = ep < ms->p_end ? *ep : 0;
    switch (suffix) {
    case '?': {  /* optional */
      const char *res;
      if (m && ((res=match(ms, s+1, ep+1)) != NULL)) {
	s = res;
	break;
      }
      p=ep+1;
      goto init;  /* else s = match(ms, s, ep+1); */
      }
    case '*':  /* 0 or more repetitions */
      s = max_expand(ms, s, p, ep);
      break;
    case '+':  /* 1 or more repetitions */
      s = (m ? max_expand(ms, s+1, p, ep) : NULL);
      break;
    case '-':  /* 0 or more repetitions (minimum) */
      s = min_expand(ms, s, p, ep);
      break;
    default:
      if (m) { s++; p=ep; goto init; }  /* else s = match(ms, s+1, ep); */
      s = NULL;
      break;
    }
    break;
    }
  }
  ms->depth--;
  return s;
}

static void push_onecapture(MatchState *ms, int i, const char *s, const char *e)
{
  if (i >= ms->level) {
    if (i == 0)  /* ms->level == 0, too */
      lua_pushlstring(ms->L, s, (size_t)(e - s));  /* add whole match */
    else
      lj_err_caller(ms->L, LJ_ERR_STRCAPI);
  } else {
    ptrdiff_t l = ms->capture[i].len;
    if (l == CAP_UNFINISHED) lj_err_caller(ms->L, LJ_ERR_STRCAPU);
    if (l == CAP_POSITION)
      lua_pushinteger(ms->L, ms->capture[i].init - ms->src_init + 1);
    else
      lua_pushlstring(ms->L, ms->capture[i].init, (size_t)l);
  }
}

static int push_captures(MatchState *ms, const char *s, const char *e)
{
  int i;
  int nlevels = (ms->level == 0 && s) ? 1 : ms->level;
  luaL_checkstack(ms->L, nlevels, "too many captures");
  for (i = 0; i < nlevels; i++)
    push_onecapture(ms, i, s, e);
  return nlevels;  /* number of strings pushed */
}

static int str_find_aux(lua_State *L, int find)
{
  GCstr *s = lj_lib_checkstr(L, 1);
  GCstr *p = lj_lib_checkstr(L, 2);
  int32_t start = lj_lib_optint(L, 3, 1);
  MSize st;
  if (start < 0) start += (int32_t)s->len; else start--;
  if (start < 0) start = 0;
  st = (MSize)start;
  if (st > s->len) {
#if LJ_52
    setnilV(L->top-1);
    return 1;
#else
    st = s->len;
#endif
  }
  if (find && ((L->base+3 < L->top && tvistruecond(L->base+3)) ||
	       !lj_str_haspattern(p))) {  /* Search for fixed string. */
    const char *q = lj_str_find(strdata(s)+st, strdata(p), s->len-st, p->len);
    if (q) {
      setintV(L->top-2, (int32_t)(q-strdata(s)) + 1);
      setintV(L->top-1, (int32_t)(q-strdata(s)) + (int32_t)p->len);
      return 2;
    }
  } else {  /* Search for pattern. */
    MatchState ms;
    const char *pattern_start = strdata(p);
    const char *pattern_end = pattern_start + p->len;
    const char *pstr = pattern_start;
    const char *sstr = strdata(s) + st;
    int anchor = 0;
    if (pstr < pattern_end && *pstr == '^') { pstr++; anchor = 1; }
    ms.L = L;
    ms.src_init = strdata(s);
    ms.src_end = strdata(s) + s->len;
    ms.p_end = pattern_end;
    do {  /* Loop through string and try to match the pattern. */
      const char *q;
      ms.level = ms.depth = 0;
      q = match(&ms, sstr, pstr);
      if (q) {
	if (find) {
	  setintV(L->top++, (int32_t)(sstr-(strdata(s)-1)));
	  setintV(L->top++, (int32_t)(q-strdata(s)));
	  return push_captures(&ms, NULL, NULL) + 2;
	} else {
	  return push_captures(&ms, sstr, q);
	}
      }
    } while (sstr++ < ms.src_end && !anchor);
  }
  setnilV(L->top-1);  /* Not found. */
  return 1;
}

LJLIB_CF(string_find)		LJLIB_REC(.)
{
  return str_find_aux(L, 1);
}

LJLIB_CF(string_match)
{
  return str_find_aux(L, 0);
}

LJLIB_NOREG LJLIB_CF(string_gmatch_aux)
{
  TValue tvpat, tvstr, tvpos;
  const char *p;
  GCstr *pat;
  GCstr *str;
  const char *s;
  const char *src;
  MatchState ms;
  lj_lib_upvalue_load_acq(L, 2, &tvpat);
  lj_lib_upvalue_load_acq(L, 1, &tvstr);
  lj_lib_upvalue_load_acq(L, 3, &tvpos);
  pat = strV(&tvpat);
  p = strdata(pat);
  str = strV(&tvstr);
  s = strdata(str);
  src = s + numberVint(&tvpos);
  ms.L = L;
  ms.src_init = s;
  ms.src_end = s + str->len;
  ms.p_end = p + pat->len;
  for (; src <= ms.src_end; src++) {
    const char *e;
    ms.level = ms.depth = 0;
    if ((e = match(&ms, src, p)) != NULL) {
      int32_t pos = (int32_t)(e - s);
      if (e == src) pos++;  /* Ensure progress for empty match. */
      setintV(&tvpos, pos);
      lj_lib_upvalue_store_prim_rel(L, 3, &tvpos);
      return push_captures(&ms, src, e);
    }
  }
  return 0;  /* not found */
}

LJLIB_CF(string_gmatch)
{
  lj_lib_checkstr(L, 1);
  lj_lib_checkstr(L, 2);
  L->top = L->base+3;
  setintV(L->top-1, 0);
  lj_lib_pushcc(L, lj_cf_string_gmatch_aux, FF_string_gmatch_aux, 3);
  return 1;
}

static void add_s(MatchState *ms, luaL_Buffer *b, const char *s, const char *e)
{
  size_t l, i;
  const char *news = lua_tolstring(ms->L, 3, &l);
  for (i = 0; i < l; i++) {
    if (news[i] != L_ESC) {
      luaL_addchar(b, news[i]);
    } else {
      i++;  /* skip ESC */
      if (!lj_char_isdigit(uchar(news[i]))) {
	luaL_addchar(b, news[i]);
      } else if (news[i] == '0') {
	luaL_addlstring(b, s, (size_t)(e - s));
      } else {
	push_onecapture(ms, news[i] - '1', s, e);
	luaL_addvalue(b);  /* add capture to accumulated result */
      }
    }
  }
}

static void add_value(MatchState *ms, luaL_Buffer *b,
		      const char *s, const char *e)
{
  lua_State *L = ms->L;
  switch (lua_type(L, 3)) {
    case LUA_TNUMBER:
    case LUA_TSTRING: {
      add_s(ms, b, s, e);
      return;
    }
    case LUA_TFUNCTION: {
      int n;
      lua_pushvalue(L, 3);
      n = push_captures(ms, s, e);
      lua_call(L, n, 1);
      break;
    }
    case LUA_TTABLE: {
      push_onecapture(ms, 0, s, e);
      lua_gettable(L, 3);
      break;
    }
  }
  if (!lua_toboolean(L, -1)) {  /* nil or false? */
    lua_pop(L, 1);
    lua_pushlstring(L, s, (size_t)(e - s));  /* keep original text */
  } else if (!lua_isstring(L, -1)) {
    lj_err_callerv(L, LJ_ERR_STRGSRV, luaL_typename(L, -1));
  }
  luaL_addvalue(b);  /* add result to accumulator */
}

LJLIB_CF(string_gsub)
{
  size_t srcl, lp;
  const char *src = luaL_checklstring(L, 1, &srcl);
  const char *pattern_start = luaL_checklstring(L, 2, &lp);
  const char *pattern_end = pattern_start + lp;
  const char *p = pattern_start;
  int  tr = lua_type(L, 3);
  int max_s = (int)luaL_optinteger(L, 4, (lua_Integer)(srcl+1));
  int anchor = p < pattern_end && *p == '^';
  int n = 0;
  MatchState ms;
  luaL_Buffer b;
  if (!(tr == LUA_TNUMBER || tr == LUA_TSTRING ||
	tr == LUA_TFUNCTION || tr == LUA_TTABLE))
    lj_err_arg(L, 3, LJ_ERR_NOSFT);
  luaL_buffinit(L, &b);
  if (anchor)
    p++;
  ms.L = L;
  ms.src_init = src;
  ms.src_end = src+srcl;
  ms.p_end = pattern_end;
  while (n < max_s) {
    const char *e;
    ms.level = ms.depth = 0;
    e = match(&ms, src, p);
    if (e) {
      n++;
      add_value(&ms, &b, src, e);
    }
    if (e && e>src) /* non empty match? */
      src = e;  /* skip it */
    else if (src < ms.src_end)
      luaL_addchar(&b, *src++);
    else
      break;
    if (anchor)
      break;
  }
  luaL_addlstring(&b, src, (size_t)(ms.src_end-src));
  luaL_pushresult(&b);
  lua_pushinteger(L, n);  /* number of substitutions */
  return 2;
}

/* ------------------------------------------------------------------------ */

LJLIB_CF(string_format)		LJLIB_REC(.)
{
  int retry = 0;
  SBuf *sb;
  do {
    sb = lj_buf_tmp_(L);
    retry = lj_strfmt_putarg(L, sb, 1, -retry);
  } while (retry > 0);
  setstrV(L, L->top-1, lj_buf_str(L, sb));
  lj_gc_check(L);
  return 1;
}

/* ------------------------------------------------------------------------ */

#ifndef LUAL_PACKPADBYTE
#define LUAL_PACKPADBYTE 0
#endif

#define PACK_MAXINTSIZE 16u
#define PACK_NB CHAR_BIT
#define PACK_INT_EXACT_MAX 9007199254740992.0

typedef struct PackHeader {
  lua_State *L;
  const char *end;
  int little;
  size_t maxalign;
} PackHeader;

typedef enum PackOption {
  PACK_INT, PACK_UINT, PACK_FLOAT, PACK_CHAR, PACK_STRING, PACK_ZSTRING,
  PACK_PADDING, PACK_ALIGN, PACK_NOP
} PackOption;

struct PackAlign {
  char c;
  union { double d; void *p; lua_Integer i; lua_Number n; } u;
};

#define PACK_NATIVE_ALIGN offsetof(struct PackAlign, u)

static void pack_error(PackHeader *h, const char *msg)
{
  luaL_argerror(h->L, 1, msg);
}

static size_t pack_getnum(PackHeader *h, const char **fmt, size_t def,
			  int required)
{
  const char *p = *fmt;
  size_t n = 0;
  if (p >= h->end || *p < '0' || *p > '9') {
    if (required) pack_error(h, "missing size for format option");
    return def;
  }
  do {
    unsigned digit = (unsigned)(*p++ - '0');
    if (n > (SIZE_MAX - digit) / 10u)
      pack_error(h, "format size overflow");
    n = n * 10u + digit;
  } while (p < h->end && *p >= '0' && *p <= '9');
  *fmt = p;
  return n;
}

static size_t pack_intsize(PackHeader *h, const char **fmt, size_t def)
{
  size_t size = pack_getnum(h, fmt, def, 0);
  if (size == 0 || size > PACK_MAXINTSIZE)
    pack_error(h, "integral size out of limits [1,16]");
  return size;
}

static int pack_ispow2(size_t n)
{
  return n != 0 && (n & (n - 1u)) == 0;
}

static PackOption pack_getoption(PackHeader *h, const char **fmt,
				 size_t *size)
{
  int opt;
  if (*fmt >= h->end)
    pack_error(h, "missing format option");
  opt = (unsigned char)*(*fmt)++;
  *size = 0;
  switch (opt) {
  case 'b': *size = sizeof(signed char); return PACK_INT;
  case 'B': *size = sizeof(unsigned char); return PACK_UINT;
  case 'h': *size = sizeof(short); return PACK_INT;
  case 'H': *size = sizeof(unsigned short); return PACK_UINT;
  case 'l': *size = sizeof(long); return PACK_INT;
  case 'L': *size = sizeof(unsigned long); return PACK_UINT;
  case 'j': *size = sizeof(lua_Integer); return PACK_INT;
  case 'J': *size = sizeof(lua_Unsigned); return PACK_UINT;
  case 'T': *size = sizeof(size_t); return PACK_UINT;
  case 'i': *size = pack_intsize(h, fmt, sizeof(int)); return PACK_INT;
  case 'I': *size = pack_intsize(h, fmt, sizeof(unsigned int)); return PACK_UINT;
  case 'f': *size = sizeof(float); return PACK_FLOAT;
  case 'd': *size = sizeof(double); return PACK_FLOAT;
  case 'n': *size = sizeof(lua_Number); return PACK_FLOAT;
  case 'c':
    *size = pack_getnum(h, fmt, 0, 1);
    if (*size > LJ_MAX_STR) pack_error(h, "format result too large");
    return PACK_CHAR;
  case 's': *size = pack_intsize(h, fmt, sizeof(size_t)); return PACK_STRING;
  case 'z': return PACK_ZSTRING;
  case 'x': *size = 1; return PACK_PADDING;
  case 'X': return PACK_ALIGN;
  case ' ': return PACK_NOP;
  case '<': h->little = 1; return PACK_NOP;
  case '>': h->little = 0; return PACK_NOP;
  case '=': h->little = LJ_LE; return PACK_NOP;
  case '!':
    h->maxalign = pack_intsize(h, fmt, PACK_NATIVE_ALIGN);
    if (!pack_ispow2(h->maxalign))
      pack_error(h, "format asks for alignment not power of 2");
    return PACK_NOP;
  default:
    luaL_error(h->L, "invalid format option '%c'", opt);
    return PACK_NOP;
  }
}

static PackOption pack_getdetails(PackHeader *h, size_t total,
				  const char **fmt, size_t *size,
				  size_t *padding)
{
  PackOption option = pack_getoption(h, fmt, size);
  size_t align = *size;
  if (option == PACK_ALIGN) {
    PackOption next;
    if (*fmt >= h->end)
      pack_error(h, "invalid next option for option 'X'");
    next = pack_getoption(h, fmt, &align);
    if (next == PACK_CHAR || align == 0)
      pack_error(h, "invalid next option for option 'X'");
  }
  if (option == PACK_CHAR || align <= 1) {
    *padding = 0;
  } else {
    if (align > h->maxalign) align = h->maxalign;
    if (!pack_ispow2(align))
      pack_error(h, "format asks for alignment not power of 2");
    *padding = (align - (total & (align - 1u))) & (align - 1u);
  }
  return option;
}

static void pack_checkadd(PackHeader *h, size_t total, size_t add)
{
  if (add > (size_t)LJ_MAX_STR - total)
    pack_error(h, "format result too large");
}

static void pack_pad(SBuf *sb, size_t n)
{
  while (n-- != 0)
    lj_buf_putchar(sb, LUAL_PACKPADBYTE);
}

static lua_Number pack_checkinteger(lua_State *L, int arg)
{
  lua_Number n = luaL_checknumber(L, arg);
  if (n != n || n < -PACK_INT_EXACT_MAX || n > PACK_INT_EXACT_MAX ||
      floor(n) != n)
    luaL_argerror(L, arg, "number has no integer representation");
  return n;
}

static void pack_integer(SBuf *sb, uint64_t value, int little, size_t size,
			 int negative)
{
  unsigned char bytes[PACK_MAXINTSIZE];
  size_t i;
  for (i = 0; i < size; i++) {
    unsigned byte = i < sizeof(value) ?
	(unsigned)((value >> (i * PACK_NB)) & U64x(00000000,000000ff)) :
	(negative ? 0xffu : 0u);
    bytes[little ? i : size - 1u - i] = (unsigned char)byte;
  }
  lj_buf_putmem(sb, bytes, (MSize)size);
}

static void pack_float(SBuf *sb, const void *value, size_t size, int little)
{
  unsigned char bytes[sizeof(lua_Number) > sizeof(double) ?
		      sizeof(lua_Number) : sizeof(double)];
  unsigned char out[sizeof(bytes)];
  size_t i;
  memcpy(bytes, value, size);
  if (little == LJ_LE) {
    lj_buf_putmem(sb, bytes, (MSize)size);
  } else {
    for (i = 0; i < size; i++) out[i] = bytes[size - 1u - i];
    lj_buf_putmem(sb, out, (MSize)size);
  }
}

static void pack_init(lua_State *L, PackHeader *h, GCstr *fmt)
{
  h->L = L;
  h->end = strdata(fmt) + fmt->len;
  h->little = LJ_LE;
  h->maxalign = 1;
}

static int string_pack(lua_State *L)
{
  GCstr *format = lj_lib_checkstr(L, 1);
  const char *fmt = strdata(format);
  PackHeader h;
  SBuf *sb = lj_buf_tmp_(L);
  size_t total = 0;
  int arg = 1;
  pack_init(L, &h, format);
  while (fmt < h.end) {
    size_t size, padding;
    PackOption option = pack_getdetails(&h, total, &fmt, &size, &padding);
    pack_checkadd(&h, total, padding + size);
    pack_pad(sb, padding);
    total += padding + size;
    switch (option) {
    case PACK_INT: {
      lua_Number d = pack_checkinteger(L, ++arg);
      uint64_t bits;
      if (size < sizeof(uint64_t)) {
        uint64_t limit = UINT64_C(1) << (size * PACK_NB - 1u);
        luaL_argcheck(L, d >= -(lua_Number)limit && d < (lua_Number)limit,
		      arg, "integer overflow");
      }
      bits = d < 0 ? (uint64_t)(int64_t)d : (uint64_t)d;
      pack_integer(sb, bits, h.little, size, d < 0);
      break;
    }
    case PACK_UINT: {
      lua_Number d = pack_checkinteger(L, ++arg);
      uint64_t bits;
      luaL_argcheck(L, d >= 0, arg, "unsigned overflow");
      bits = (uint64_t)d;
      if (size < sizeof(uint64_t)) {
        uint64_t limit = UINT64_C(1) << (size * PACK_NB);
        luaL_argcheck(L, bits < limit, arg, "unsigned overflow");
      }
      pack_integer(sb, bits, h.little, size, 0);
      break;
    }
    case PACK_FLOAT: {
      lua_Number n = luaL_checknumber(L, ++arg);
      if (size == sizeof(float)) {
        float f = (float)n;
        pack_float(sb, &f, size, h.little);
      } else if (size == sizeof(double)) {
        double d = (double)n;
        pack_float(sb, &d, size, h.little);
      } else {
        pack_float(sb, &n, size, h.little);
      }
      break;
    }
    case PACK_CHAR: {
      GCstr *s = lj_lib_checkstr(L, ++arg);
      luaL_argcheck(L, s->len <= size, arg,
		    "string longer than given size");
      lj_buf_putmem(sb, strdata(s), s->len);
      pack_pad(sb, size - s->len);
      break;
    }
    case PACK_STRING: {
      GCstr *s = lj_lib_checkstr(L, ++arg);
      if (size < sizeof(uint64_t)) {
        uint64_t limit = UINT64_C(1) << (size * PACK_NB);
        luaL_argcheck(L, (uint64_t)s->len < limit, arg,
		      "string length does not fit in given size");
      }
      pack_checkadd(&h, total, s->len);
      pack_integer(sb, (uint64_t)s->len, h.little, size, 0);
      lj_buf_putmem(sb, strdata(s), s->len);
      total += s->len;
      break;
    }
    case PACK_ZSTRING: {
      GCstr *s = lj_lib_checkstr(L, ++arg);
      luaL_argcheck(L, memchr(strdata(s), 0, s->len) == NULL, arg,
		    "string contains zeros");
      pack_checkadd(&h, total, (size_t)s->len + 1u);
      lj_buf_putmem(sb, strdata(s), s->len);
      lj_buf_putchar(sb, 0);
      total += (size_t)s->len + 1u;
      break;
    }
    case PACK_PADDING:
      lj_buf_putchar(sb, LUAL_PACKPADBYTE);
      break;
    case PACK_ALIGN:
    case PACK_NOP:
      break;
    }
  }
  setstrV(L, L->top-1, lj_buf_str(L, sb));
  lj_state_stack_pubtv(L, L, L->top-1);
  lj_gc_check(L);
  return 1;
}

static int string_packsize(lua_State *L)
{
  GCstr *format = lj_lib_checkstr(L, 1);
  const char *fmt = strdata(format);
  PackHeader h;
  size_t total = 0;
  pack_init(L, &h, format);
  while (fmt < h.end) {
    size_t size, padding;
    PackOption option = pack_getdetails(&h, total, &fmt, &size, &padding);
    if (option == PACK_STRING || option == PACK_ZSTRING)
      pack_error(&h, "variable-length format");
    pack_checkadd(&h, total, padding + size);
    total += padding + size;
  }
  lua_pushnumber(L, (lua_Number)total);
  return 1;
}

static uint64_t unpack_integer(PackHeader *h, const unsigned char *data,
			       size_t size, int sign)
{
  uint64_t value = 0;
  size_t used = size < sizeof(value) ? size : sizeof(value);
  size_t i;
  for (i = 0; i < used; i++) {
    size_t at = h->little ? i : size - 1u - i;
    value |= (uint64_t)data[at] << (i * PACK_NB);
  }
  if (sign && size < sizeof(value) &&
      (value & (UINT64_C(1) << (size * PACK_NB - 1u))))
    value |= UINT64_MAX << (size * PACK_NB);
  if (size > sizeof(value)) {
    unsigned ext = sign && (value & (UINT64_C(1) << 63)) ? 0xffu : 0u;
    for (i = sizeof(value); i < size; i++) {
      size_t at = h->little ? i : size - 1u - i;
      if (data[at] != ext)
        luaL_error(h->L, "%d-byte integer does not fit into Lua Integer",
		   (int)size);
    }
  }
  return value;
}

static void unpack_float(void *value, const unsigned char *data, size_t size,
			 int little)
{
  unsigned char bytes[sizeof(lua_Number) > sizeof(double) ?
		      sizeof(lua_Number) : sizeof(double)];
  size_t i;
  if (little == LJ_LE)
    memcpy(bytes, data, size);
  else
    for (i = 0; i < size; i++) bytes[i] = data[size - 1u - i];
  memcpy(value, bytes, size);
}

static int string_unpack(lua_State *L)
{
  GCstr *format = lj_lib_checkstr(L, 1);
  GCstr *datastr = lj_lib_checkstr(L, 2);
  const char *fmt = strdata(format);
  const unsigned char *data = (const unsigned char *)strdata(datastr);
  int64_t initial = (L->base+2 >= L->top || tvisnil(L->base+2)) ?
	1 : (int64_t)pack_checkinteger(L, 3);
  int64_t signedpos;
  size_t pos;
  int nresult = 0;
  PackHeader h;
  if (initial < 0)
    signedpos = (int64_t)datastr->len + (int64_t)initial;
  else if (initial > 0)
    signedpos = (int64_t)initial - 1;
  else
    signedpos = -1;
  luaL_argcheck(L, signedpos >= 0 && (uint64_t)signedpos <= datastr->len,
		3, "initial position out of string");
  pos = (size_t)signedpos;
  pack_init(L, &h, format);
  while (fmt < h.end) {
    size_t size, padding;
    PackOption option = pack_getdetails(&h, pos, &fmt, &size, &padding);
    if (padding > (size_t)datastr->len - pos ||
	size > (size_t)datastr->len - pos - padding)
      luaL_argerror(L, 2, "data string too short");
    pos += padding;
    if (option == PACK_INT || option == PACK_UINT || option == PACK_FLOAT ||
	option == PACK_CHAR || option == PACK_STRING || option == PACK_ZSTRING) {
      if (nresult >= LUAI_MAXCSTACK - 1)
	luaL_error(L, "too many results");
      luaL_checkstack(L, 2, "too many results");
      nresult++;
    }
    switch (option) {
    case PACK_INT:
    case PACK_UINT: {
      uint64_t bits = unpack_integer(&h, data + pos, size,
				     option == PACK_INT);
      if (option == PACK_INT) {
	int64_t value = (bits & (UINT64_C(1) << 63)) ?
	  -1 - (int64_t)(~bits) : (int64_t)bits;
	if (value < -INT64_C(9007199254740992) ||
	    value > INT64_C(9007199254740992))
	  luaL_error(L, "%d-byte integer does not fit into Lua Integer",
		     (int)size);
	lua_pushnumber(L, (lua_Number)value);
      } else {
	if (bits > UINT64_C(9007199254740992))
	  luaL_error(L, "%d-byte integer does not fit into Lua Integer",
		     (int)size);
	lua_pushnumber(L, (lua_Number)bits);
      }
      break;
    }
    case PACK_FLOAT: {
      lua_Number n;
      if (size == sizeof(float)) {
        float f;
        unpack_float(&f, data + pos, size, h.little);
        n = (lua_Number)f;
      } else if (size == sizeof(double)) {
        double d;
        unpack_float(&d, data + pos, size, h.little);
        n = (lua_Number)d;
      } else {
        unpack_float(&n, data + pos, size, h.little);
      }
      lua_pushnumber(L, n);
      break;
    }
    case PACK_CHAR:
      lua_pushlstring(L, (const char *)data + pos, size);
      break;
    case PACK_STRING: {
      uint64_t length = unpack_integer(&h, data + pos, size, 0);
      if (length > (uint64_t)((size_t)datastr->len - pos - size))
	luaL_argerror(L, 2, "data string too short");
      lua_pushlstring(L, (const char *)data + pos + size, (size_t)length);
      pos += (size_t)length;
      break;
    }
    case PACK_ZSTRING: {
      const unsigned char *zero = (const unsigned char *)memchr(
	data + pos, 0, (size_t)datastr->len - pos);
      size_t length;
      if (zero == NULL)
	luaL_argerror(L, 2, "unfinished string for format 'z'");
      length = (size_t)(zero - (data + pos));
      lua_pushlstring(L, (const char *)data + pos, length);
      pos += length + 1u;
      break;
    }
    case PACK_PADDING:
    case PACK_ALIGN:
    case PACK_NOP:
      break;
    }
    pos += size;
  }
  lua_pushnumber(L, (lua_Number)pos + 1);
  return nresult + 1;
}

/* ------------------------------------------------------------------------ */

#include "lj_libdef.h"

static const luaL_Reg string_compat53[] = {
  { "pack", string_pack },
  { "packsize", string_packsize },
  { "unpack", string_unpack },
  { NULL, NULL }
};

static void string_storetab_str(lua_State *L, GCtab *tab, GCstr *key,
				GCtab *val)
{
  TValue keytv, tv, *dst;
  settabV(L, &tv, val);
  setstrV(L, &keytv, key);
  for (;;) {
    dst = lj_tab_setstr(L, tab, key);
    if (lj_tab_trystoretv_cas_keyed(L, tab, dst, &keytv, &tv) ==
	LJ_TAB_STORE_CAS_OK)
      return;
    lj_tab_store_wait_l(L);  /* string metatable store saw stale/FORWARD slot. */
  }
}

LUALIB_API int luaopen_string(lua_State *L)
{
  GCtab *mt, *strtab;
  global_State *g;
  LJ_LIB_REG(L, LUA_STRLIBNAME, string);
  luaL_setfuncs(L, string_compat53, 0);
  strtab = tabV(L->top-1);
  lj_state_checkstack(L, 1);
  mt = lj_tab_new(L, 0, 1);
  settabV(L, L->top, mt);
  lj_state_stack_pubtv(L, L, L->top);
  L->top++;
  g = G(L);
  string_storetab_str(L, mt, mmname_str(g, MM_index), strtab);
  lj_tab_nomm_rel(mt, (uint8_t)(~(1u<<MM_index)));
  lj_gc_pubtabobj(L, mt, strtab);
  /* NOBARRIER: basemt is a GC root. */
  lj_basemt_it_rel(g, LJ_TSTR, mt);
  L->top--;
#if LJ_HASBUFFER
  lj_lib_prereg(L, LUA_STRLIBNAME ".buffer", luaopen_string_buffer, strtab);
#endif
  return 1;
}
