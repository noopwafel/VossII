//-------------------------------------------------------------------
// Copyright 2020 Carl-Johan Seger
// SPDX-License-Identifier: Apache-2.0
//-------------------------------------------------------------------

/************************************************************************/
/*									*/
/*		Original author: Carl-Johan Seger, 2017			*/
/*									*/
/************************************************************************/
#include "strings.h"
#include "graph.h"
#include <limits.h>
#include <stdlib.h>
#include <fnmatch.h>
#include <ctype.h>

/* ------------- Global variables ------------- */

/********* Global variables referenced ***********/
extern str_mgr     strings;
extern jmp_buf     *start_envp;

/***** PRIVATE VARIABLES *****/
static char             path_buf[PATH_MAX+1];
static ustr_mgr	    lstrings;
static ustr_mgr	    *lstringsp;
static rec_mgr	    vec_rec_mgr;
static rec_mgr	    *vec_rec_mgrp;
static rec_mgr	    range_rec_mgr;
static rec_mgr	    *range_rec_mgrp;
static rec_mgr	    sname_list_rec_mgr;
static rec_mgr	    *sname_list_rec_mgrp;
static rec_mgr	    merge_list_rec_mgr;
static rec_mgr	    *merge_list_rec_mgrp;

/* ----- Forward definitions local functions ----- */
static void		begin_vector_ops();
static void		end_vector_ops();
static range_ptr	make_range_canonical(range_ptr rp);
static void		emit_vector(vec_ptr vp, bool non_contig_vecs,
				    g_ptr *tailp);
static bool		same_range(range_ptr r1, range_ptr r2);
static range_ptr	compress_ranges(range_ptr r1, range_ptr r2);
static vec_ptr		split_name(string name);
static sname_list_ptr   *prepend_index(int idx, sname_list_ptr rem,
				       sname_list_ptr *resp);
static sname_list_ptr	expand_vec(vec_ptr vec);
static int		vec_name_cmp(vec_ptr v1, vec_ptr v2);
static int		nn_cmp(const void *pi, const void *pj);
static void		gen_extract_vectors(g_ptr redex, bool non_contig_vecs);
static void		gen_merge_vectors(g_ptr redex, g_ptr l,
					  bool non_contig_vecs);
static int		vec_size(vec_ptr vec);

/********************************************************/
/*                    PUBLIC FUNCTIONS    		*/
/********************************************************/

static string		mk_name_signature(vec_ptr vp);


void
Strings_Init()
{
    // Any needed initialization code
}

vec_ptr
Split_vector_name(ustr_mgr *str_mgr_ptr, 
		  rec_mgr  *vector_mgrp,
		  rec_mgr  *range_mgrp,
		  string vec)
{
    ustr_mgr *tmp_lstringsp = lstringsp;
    rec_mgr *tmp_vec_rec_mgrp = vec_rec_mgrp;
    rec_mgr *tmp_range_rec_mgrp = range_rec_mgrp;
    lstringsp = str_mgr_ptr;
    vec_rec_mgrp = vector_mgrp;
    range_rec_mgrp = range_mgrp;
    vec_ptr res = split_name(vec);
    lstringsp = tmp_lstringsp;
    vec_rec_mgrp = tmp_vec_rec_mgrp;
    range_rec_mgrp = tmp_range_rec_mgrp;
    return res;
}

string
Get_vector_signature(ustr_mgr *str_mgr_ptr, vec_ptr vp)
{
    ustr_mgr *tmp = lstringsp;
    lstringsp = str_mgr_ptr;
    string res = mk_name_signature(vp);
    lstringsp = tmp;
    return res;
}

g_ptr
Vec2nodes(string name)
{
    begin_vector_ops();
    g_ptr res = Make_NIL();
    g_ptr tail = res;
    vec_ptr vp = split_name(name);
    sname_list_ptr nlp = expand_vec(vp);
    while( nlp != NULL ) {
	SET_CONS_HD(tail, Make_STRING_leaf(wastrsave(&strings, nlp->name)));
	SET_CONS_TL(tail, Make_NIL());
	tail = GET_CONS_TL(tail);
	nlp = nlp->next;
    }
    end_vector_ops();
    return res;
}

int
Get_Vector_Size(string vec)
{
    begin_vector_ops();
    vec_ptr vp = split_name(vec);
    int sz = vec_size(vp);
    end_vector_ops();
    return sz;
}

g_ptr
Merge_Vectors(g_ptr nds, bool non_contig_vecs)
{
    g_ptr res = Get_node();
    gen_merge_vectors(res, nds, non_contig_vecs);
    return res;
}

/********************************************************/
/*	    EXPORTED EXTAPI FUNCTIONS    		*/
/********************************************************/

static void
file_fullname(g_ptr redex)
{
    g_ptr l = GET_APPLY_LEFT(redex);
    g_ptr r = GET_APPLY_RIGHT(redex);
    string tmp = realpath(GET_STRING(r), path_buf);
    if( tmp == NULL ) {
	MAKE_REDEX_FAILURE(redex,
			   Fail_pr("Cannot find file %s\n", GET_STRING(r)));
	return;
    }
    MAKE_REDEX_STRING(redex, wastrsave(&strings, tmp));
    DEC_REF_CNT(l);
    DEC_REF_CNT(r);
}

static void
do_strlen(g_ptr redex)
{
    g_ptr l = GET_APPLY_LEFT(redex);
    g_ptr r = GET_APPLY_RIGHT(redex);
    MAKE_REDEX_INT(redex, strlen(GET_STRING(r)));
    DEC_REF_CNT(l);
    DEC_REF_CNT(r);
}

static void
do_strcmp(g_ptr redex)
{
    g_ptr l = GET_APPLY_LEFT(redex);
    g_ptr r = GET_APPLY_RIGHT(redex);
    g_ptr arg1 = GET_APPLY_RIGHT(l);
    g_ptr arg2 = GET_APPLY_RIGHT(redex);
    string s1  = GET_STRING(arg1);
    string s2  = GET_STRING(arg2);
    int res = strcmp(s1,s2);
    MAKE_REDEX_INT(redex, res);
    DEC_REF_CNT(l);
    DEC_REF_CNT(r);
}

static void
str_is_prefix(g_ptr redex)
{
    g_ptr l = GET_APPLY_LEFT(redex);
    g_ptr r = GET_APPLY_RIGHT(redex);
    g_ptr arg1 = GET_APPLY_RIGHT(l);
    g_ptr arg2 = GET_APPLY_RIGHT(redex);
    string pre = GET_STRING(arg1);
    string s   = GET_STRING(arg2);
    if( strncmp(pre, s, strlen(pre)) == 0 ) {
	MAKE_REDEX_BOOL(redex, B_One());
    } else {
	MAKE_REDEX_BOOL(redex, B_Zero());
    }
    DEC_REF_CNT(l);
    DEC_REF_CNT(r);
}

static void
str_is_suffix(g_ptr redex)
{
    g_ptr l = GET_APPLY_LEFT(redex);
    g_ptr r = GET_APPLY_RIGHT(redex);
    g_ptr arg1 = GET_APPLY_RIGHT(l);
    g_ptr arg2 = GET_APPLY_RIGHT(redex);
    string suf = GET_STRING(arg1);
    string s   = GET_STRING(arg2);
    int lsuf = strlen(suf);
    int ls   = strlen(s);
    if( lsuf <= ls && strcmp(s+(ls-lsuf), suf) == 0 ) {
	MAKE_REDEX_BOOL(redex, B_One());
    } else {
	MAKE_REDEX_BOOL(redex, B_Zero());
    }
    DEC_REF_CNT(l);
    DEC_REF_CNT(r);
}

static void
str_is_substr(g_ptr redex)
{
    g_ptr l = GET_APPLY_LEFT(redex);
    g_ptr r = GET_APPLY_RIGHT(redex);
    g_ptr arg1 = GET_APPLY_RIGHT(l);
    g_ptr arg2 = GET_APPLY_RIGHT(redex);
    string needle = GET_STRING(arg1);
    string haystack   = GET_STRING(arg2);
    if( strstr(haystack, needle) != NULL ) {
	MAKE_REDEX_BOOL(redex, B_One());
    } else {
	MAKE_REDEX_BOOL(redex, B_Zero());
    }
    DEC_REF_CNT(l);
    DEC_REF_CNT(r);
}

static void
str_match(g_ptr redex)
{
    g_ptr l    = GET_APPLY_LEFT(redex);
    g_ptr r    = GET_APPLY_RIGHT(redex);
    g_ptr arg1 = GET_APPLY_RIGHT(l);
    g_ptr arg2 = GET_APPLY_RIGHT(redex);
    string pat = GET_STRING(arg1);
    string s   = GET_STRING(arg2);
    if( fnmatch(pat, s, 0) == 0 ) {
	MAKE_REDEX_BOOL(redex, B_One());
    } else {
	MAKE_REDEX_BOOL(redex, B_Zero());
    }
    DEC_REF_CNT(l);
    DEC_REF_CNT(r);
}


static void
str_split(g_ptr redex)
{
    g_ptr l = GET_APPLY_LEFT(redex);
    g_ptr r = GET_APPLY_RIGHT(redex);
    g_ptr arg1       = GET_APPLY_RIGHT(l);
    g_ptr arg2       = GET_APPLY_RIGHT(redex);
    string s         = GET_STRING(arg1);
    string split_str = GET_STRING(arg2);
    int lsplit = strlen(split_str);
    string tmp = strtemp(s);
    string cur = tmp;
    string next;
    MAKE_REDEX_NIL(redex);
    g_ptr tail = redex;
    if( lsplit == 0 ) {
	char buf[2];
	buf[1] = 0;
	while( *s != 0) {
	    buf[0] = *s;
	    SET_CONS_HD(tail, Make_STRING_leaf(wastrsave(&strings, buf)));
	    SET_CONS_TL(tail, Make_NIL());
	    tail = GET_CONS_TL(tail);
	    s++;
	}
    } else {
	while( (next = strstr(cur, split_str)) != NULL ) {
	    *next = 0;
	    SET_CONS_HD(tail, Make_STRING_leaf(wastrsave(&strings, cur)));
	    SET_CONS_TL(tail, Make_NIL());
	    tail = GET_CONS_TL(tail);
	    cur = next+lsplit;
	}
	if( *cur != 0 ) {
	    SET_CONS_HD(tail, Make_STRING_leaf(wastrsave(&strings, cur)));
	    SET_CONS_TL(tail, Make_NIL());
	    tail = GET_CONS_TL(tail);
	}
    }
    DEC_REF_CNT(l);
    DEC_REF_CNT(r);
}

static void
md_expand_vectors(g_ptr redex)
{
    g_ptr l = GET_APPLY_LEFT(redex);
    g_ptr r = GET_APPLY_RIGHT(redex);
    begin_vector_ops();
    MAKE_REDEX_NIL(redex);
    g_ptr tail = redex;
    for(g_ptr np = r; !IS_NIL(np); np = GET_CONS_TL(np)) {
	vec_ptr vp = split_name(GET_STRING(GET_CONS_HD(np)));
	sname_list_ptr nlp = expand_vec(vp);
	while( nlp != NULL ) {
	    SET_CONS_HD(tail, Make_STRING_leaf(wastrsave(&strings, nlp->name)));
	    SET_CONS_TL(tail, Make_NIL());
	    tail = GET_CONS_TL(tail);
	    nlp = nlp->next;
	}
    }
    end_vector_ops();
    DEC_REF_CNT(l);
    DEC_REF_CNT(r);
}

static void
md_expand_vector(g_ptr redex)
{
    g_ptr l = GET_APPLY_LEFT(redex);
    g_ptr r = GET_APPLY_RIGHT(redex);
    begin_vector_ops();
    MAKE_REDEX_NIL(redex);
    g_ptr tail = redex;
    vec_ptr vp = split_name(GET_STRING(r));
    sname_list_ptr nlp = expand_vec(vp);
    while( nlp != NULL ) {
	SET_CONS_HD(tail, Make_STRING_leaf(wastrsave(&strings, nlp->name)));
	SET_CONS_TL(tail, Make_NIL());
	tail = GET_CONS_TL(tail);
	nlp = nlp->next;
    }
    end_vector_ops();
    DEC_REF_CNT(l);
    DEC_REF_CNT(r);
}

static void
md_size(g_ptr redex)
{
    g_ptr l = GET_APPLY_LEFT(redex);
    g_ptr r = GET_APPLY_RIGHT(redex);
    int sz = Get_Vector_Size(GET_STRING(r));
    MAKE_REDEX_INT(redex, sz);
    DEC_REF_CNT(l);
    DEC_REF_CNT(r);
}

static void
md_extract_vectors(g_ptr redex)
{
    gen_extract_vectors(redex, TRUE);
}

static void
extract_vectors(g_ptr redex)
{
    gen_extract_vectors(redex, FALSE);
}

static void
md_merge_vectors(g_ptr redex)
{
    g_ptr l = GET_APPLY_LEFT(redex);
    g_ptr r = GET_APPLY_RIGHT(redex);
    gen_merge_vectors(redex, r, TRUE);
    DEC_REF_CNT(l);
    DEC_REF_CNT(r);
}

static void
merge_vectors(g_ptr redex)
{
    g_ptr l = GET_APPLY_LEFT(redex);
    g_ptr r = GET_APPLY_RIGHT(redex);
    gen_merge_vectors(redex, r, FALSE);
    DEC_REF_CNT(l);
    DEC_REF_CNT(r);
}


static void
string_lastn(g_ptr redex)
{
    g_ptr l = GET_APPLY_LEFT(redex);
    g_ptr r = GET_APPLY_RIGHT(redex);
    string s = GET_STRING(GET_APPLY_RIGHT(l));
    int cnt = GET_INT(r);
    string tmp = strtemp(s);
    int len = strlen(s);
    if( len <= cnt ) {
	MAKE_REDEX_STRING(redex, wastrsave(&strings, tmp));
    } else {
	tmp = tmp+len-cnt;
	MAKE_REDEX_STRING(redex, wastrsave(&strings, tmp));
    }
    DEC_REF_CNT(l);
    DEC_REF_CNT(r);
}

static void
string_butlastn(g_ptr redex)
{
    g_ptr l = GET_APPLY_LEFT(redex);
    g_ptr r = GET_APPLY_RIGHT(redex);
    string s = GET_STRING(GET_APPLY_RIGHT(l));
    int cnt = GET_INT(r);
    string tmp = strtemp(s);
    int len = strlen(s);
    if( len <= cnt ) {
	MAKE_REDEX_STRING(redex, wastrsave(&strings, ""));
    } else {
	*(tmp+len-cnt) = 0;
	MAKE_REDEX_STRING(redex, wastrsave(&strings, tmp));
    }
    DEC_REF_CNT(l);
    DEC_REF_CNT(r);
}

static void
string_strstr(g_ptr redex)
{
    g_ptr l    = GET_APPLY_LEFT(redex);
    g_ptr r    = GET_APPLY_RIGHT(redex);
    g_ptr arg1 = GET_APPLY_RIGHT(l);
    g_ptr arg2 = GET_APPLY_RIGHT(redex);
    string haystack = GET_STRING(arg1);
    string needle   = GET_STRING(arg2);
    string loc = strstr(haystack, needle);
    if( loc == NULL ) {
	MAKE_REDEX_INT(redex, (0));
    } else {
	MAKE_REDEX_INT(redex, (1+loc-haystack));
    }
    DEC_REF_CNT(l);
    DEC_REF_CNT(r);
}

static void
lift_node_name_cmp(g_ptr redex)
{
    g_ptr l    = GET_APPLY_LEFT(redex);
    g_ptr r    = GET_APPLY_RIGHT(redex);
    g_ptr g_s1, g_s2;
    EXTRACT_2_ARGS(redex, g_s1, g_s2);
    string s1 = GET_STRING(g_s1);
    string s2 = GET_STRING(g_s2);
    int res = node_name_cmp(s1, s2);
    MAKE_REDEX_INT(redex, res);
    DEC_REF_CNT(l);
    DEC_REF_CNT(r);
}

static void
string_substr(g_ptr redex)
{
    g_ptr l    = GET_APPLY_LEFT(redex);
    g_ptr r    = GET_APPLY_RIGHT(redex);
    g_ptr g_s, g_f, g_c;
    EXTRACT_3_ARGS(redex, g_s, g_f, g_c);
    string s = GET_STRING(g_s); 
    int loc  = GET_INT(g_f); 
    int cnt  = GET_INT(g_c); 
    if( loc < 1 ) {
	string msg =
	    Fail_pr("Start position for str_substr before start of string (%d)",
		    loc);
	MAKE_REDEX_FAILURE(redex, msg);
	DEC_REF_CNT(l);
	DEC_REF_CNT(r);
	return;
    }
    int len  = strlen(s);
    if( loc > len ) {
	MAKE_REDEX_STRING(redex, wastrsave(&strings, ""));
	DEC_REF_CNT(l);
	DEC_REF_CNT(r);
	return;
    }
    if( cnt < 0 ) {
	string res = wastrsave(&strings, s+loc-1);
	MAKE_REDEX_STRING(redex, res);
	DEC_REF_CNT(l);
	DEC_REF_CNT(r);
	return;
    }
    if( (loc + cnt - 1) > len ) {
	string msg = Fail_pr(
      "loc (%d) plus cnt (%d) greater than length of string (%d) in str_substr", 
			loc, cnt, len);
	MAKE_REDEX_FAILURE(redex, msg);
	DEC_REF_CNT(l);
	DEC_REF_CNT(r);
	return;
    }
    string tmp = strtemp(s+loc-1);
    *(tmp+cnt) = '\0';
    string res = wastrsave(&strings, tmp);
    MAKE_REDEX_STRING(redex, res);
    DEC_REF_CNT(l);
    DEC_REF_CNT(r);
    return;
}

static void
string_trim(g_ptr redex)
{
    g_ptr l    = GET_APPLY_LEFT(redex);
    g_ptr r    = GET_APPLY_RIGHT(redex);
    g_ptr g_l, g_r, g_s;
    EXTRACT_3_ARGS(redex, g_l, g_r, g_s);
    string s = GET_STRING(g_s); 
    string pref = GET_STRING(g_l);
    string suff = GET_STRING(g_r);
    string lloc = strstr(s, pref);
    if( lloc == NULL ) {
	string msg = Fail_pr("trim: Cannot find prefix (%s) in string (%s)",
			     pref, s);
	MAKE_REDEX_FAILURE(redex, msg);
	DEC_REF_CNT(l);
	DEC_REF_CNT(r);
	return;
    }
    s = strtemp(lloc + strlen(pref));
    string cur = s;
    string found = strstr(cur, suff);
    string last = found;
    while( found != NULL ) {
	last = found;
	found = strstr(found+1, suff);
    }
    if( last == NULL ) {
	string msg = Fail_pr("trim: Cannot find suffix (%s) in string (%s)",
			     suff, s);
	MAKE_REDEX_FAILURE(redex, msg);
	DEC_REF_CNT(l);
	DEC_REF_CNT(r);
	return;
    }
    *last = 0;
    string res = wastrsave(&strings, s);
    MAKE_REDEX_STRING(redex, res);
    DEC_REF_CNT(l);
    DEC_REF_CNT(r);
    return;
}


static void
string_str_cluster(g_ptr redex)
{
    g_ptr l    = GET_APPLY_LEFT(redex);
    g_ptr r    = GET_APPLY_RIGHT(redex);
    g_ptr arg1 = GET_APPLY_RIGHT(l);
    g_ptr arg2 = GET_APPLY_RIGHT(redex);
    string s = GET_STRING(arg1);
    int  sz  = GET_INT(arg2);
    int len  = strlen(s);
    if( (len % sz) != 0 ) {
	MAKE_REDEX_FAILURE(redex,
		Fail_pr("str_cluster: string length not a multiple of %d", sz));
	DEC_REF_CNT(l);
	DEC_REF_CNT(r);
	return;
    }
    MAKE_REDEX_NIL(redex);
    string t = strtemp(s);
    g_ptr tail = redex;
    while( *t ) {
	char c = *(t+sz);
	*(t+sz) = 0;
	string item = wastrsave(&strings, t);
	SET_CONS_HD(tail, Make_STRING_leaf(item));
        SET_CONS_TL(tail, Make_NIL());
        tail = GET_CONS_TL(tail);
	t += sz;
	*t = c;
    }
    DEC_REF_CNT(l);
    DEC_REF_CNT(r);
}



void
Strings_Install_Functions()
{
    // Add builtin functions
    Add_ExtAPI_Function("file_fullname", "1", FALSE,
			GLmake_arrow(GLmake_string(),GLmake_string()),
			file_fullname);

    Add_ExtAPI_Function("strlen", "1", FALSE,
			GLmake_arrow(GLmake_string(),GLmake_int()),
			do_strlen);

    Add_ExtAPI_Function("strcmp", "11", FALSE,
			GLmake_arrow(GLmake_string(),
				     GLmake_arrow(GLmake_string(),
						  GLmake_int())),
			do_strcmp);

    Add_ExtAPI_Function("str_is_prefix", "11", FALSE,
			GLmake_arrow(GLmake_string(),
				     GLmake_arrow(GLmake_string(),
						  GLmake_bool())),
			str_is_prefix);

    Add_ExtAPI_Function("str_is_suffix", "11", FALSE,
			GLmake_arrow(GLmake_string(),
				     GLmake_arrow(GLmake_string(),
						  GLmake_bool())),
			str_is_suffix);

    Add_ExtAPI_Function("str_is_substr", "11", FALSE,
			GLmake_arrow(GLmake_string(),
				     GLmake_arrow(GLmake_string(),
						  GLmake_bool())),
			str_is_substr);

    Add_ExtAPI_Function("str_split", "11", FALSE,
			GLmake_arrow(
			    GLmake_string(),
			    GLmake_arrow(GLmake_string(),
					 GLmake_list(GLmake_string()))),
			str_split);

    Add_ExtAPI_Function("str_match", "11", FALSE,
			GLmake_arrow(
			    GLmake_string(),
			    GLmake_arrow(GLmake_string(),
					 GLmake_bool())),
			str_match);

    Add_ExtAPI_Function("md_expand_vector", "1", FALSE,
			GLmake_arrow(GLmake_string(),
				     GLmake_list(GLmake_string())),
			md_expand_vector);

    Add_ExtAPI_Function("md_expand_vectors", "1", FALSE,
			GLmake_arrow(GLmake_list(GLmake_string()),
				     GLmake_list(GLmake_string())),
			md_expand_vectors);

    Add_ExtAPI_Function("md_size", "1", FALSE,
			GLmake_arrow(GLmake_string(), GLmake_int()),
			md_size);

    Add_ExtAPI_Function("md_extract_vectors", "1", FALSE,
			GLmake_arrow(GLmake_list(GLmake_string()),
				     GLmake_list(GLmake_string())),
			md_extract_vectors);

    Add_ExtAPI_Function("extract_vectors", "1", FALSE,
			GLmake_arrow(GLmake_list(GLmake_string()),
				     GLmake_list(GLmake_string())),
			extract_vectors);

    Add_ExtAPI_Function("md_merge_vectors", "1", FALSE,
			GLmake_arrow(GLmake_list(GLmake_string()),
				     GLmake_list(GLmake_string())),
			md_merge_vectors);

    Add_ExtAPI_Function("merge_vectors", "1", FALSE,
			GLmake_arrow(GLmake_list(GLmake_string()),
				     GLmake_list(GLmake_string())),
			merge_vectors);

    Add_ExtAPI_Function("string_lastn", "11", FALSE,
			GLmake_arrow(
			    GLmake_string(),
			    GLmake_arrow(GLmake_int(),
					 GLmake_string())),
			string_lastn);

    Add_ExtAPI_Function("string_butlastn", "11", FALSE,
			GLmake_arrow(
			    GLmake_string(),
			    GLmake_arrow(GLmake_int(),
					 GLmake_string())),
			string_butlastn);

    Add_ExtAPI_Function("strstr", "11", FALSE,
			GLmake_arrow(GLmake_string(),
				     GLmake_arrow(GLmake_string(),
						  GLmake_int())),
			string_strstr);

    Add_ExtAPI_Function("substr", "111", FALSE,
			GLmake_arrow(GLmake_string(),
				     GLmake_arrow(GLmake_int(),
						  GLmake_arrow(
							    GLmake_int(),
							    GLmake_string()))),
			string_substr);

    Add_ExtAPI_Function("trim", "111", FALSE,
			GLmake_arrow(GLmake_string(),
				     GLmake_arrow(GLmake_string(),
						  GLmake_arrow(
							    GLmake_string(),
							    GLmake_string()))),
			string_trim);

    Add_ExtAPI_Function("str_cluster", "11", FALSE,
			GLmake_arrow(GLmake_string(),
				     GLmake_arrow(
					    GLmake_int(),
					    GLmake_list(GLmake_string()))),
			string_str_cluster);

    Add_ExtAPI_Function("node_name_cmp", "11", FALSE,
			GLmake_arrow(GLmake_string(),
				     GLmake_arrow(
					    GLmake_string(),
					    GLmake_int())),
			lift_node_name_cmp);



}

/********************************************************/
/*                    LOCAL FUNCTIONS    		*/
/********************************************************/

static void
begin_vector_ops()
{
    lstringsp = &lstrings;
    vec_rec_mgrp = &vec_rec_mgr;
    range_rec_mgrp = &range_rec_mgr;
    sname_list_rec_mgrp = &sname_list_rec_mgr;
    merge_list_rec_mgrp = &merge_list_rec_mgr;

    new_ustrmgr(lstringsp);
    new_mgr(vec_rec_mgrp, sizeof(vec_rec));
    new_mgr(range_rec_mgrp, sizeof(range_rec));
    new_mgr(sname_list_rec_mgrp, sizeof(sname_list_rec));
    new_mgr(merge_list_rec_mgrp, sizeof(merge_list_rec));
}

static void
end_vector_ops()
{
    free_ustrmgr(lstringsp);
    free_mgr(vec_rec_mgrp);
    free_mgr(range_rec_mgrp);
    free_mgr(sname_list_rec_mgrp);
    free_mgr(merge_list_rec_mgrp);
    lstringsp = NULL;
    vec_rec_mgrp = NULL;
    range_rec_mgrp = NULL;
    sname_list_rec_mgrp = NULL;
    merge_list_rec_mgrp = NULL;
}


static range_ptr
make_range_canonical(range_ptr rp)
{
    if( rp == NULL ) { return NULL; }
    if( rp->upper < rp->lower ) { 
	int tmp = rp->upper;
	rp->upper = rp->lower;
	rp->lower = tmp;
    }
    range_ptr rem = make_range_canonical(rp->next);
    if( rem == NULL ) {
	return rp;
    }
    if( rp->lower > rem->upper ) {
	rp->next = rem;
	return rp;
    }
    // Overlap
    if( rp->upper <= rem->upper ) {
	if( rp->lower >= rem->lower ) {
	    return rem;
	}
	rem->lower = rp->lower;
	return( make_range_canonical(rem) );
    }
    rem->upper = rp->upper;
    if( rem->lower > rp->lower ) {
	rem->lower = rp->lower;
	return( make_range_canonical(rem) );
    }
    return rem;
}


static void
emit_vector(vec_ptr vp, bool non_contig_vecs, g_ptr *tailp)
{
    string start_buf = strappend("");
    while( vp != NULL ) {
	if( vp->type == TXT ) {
	    strappend(vp->u.name);
	} else {
	    range_ptr rp = vp->u.ranges;
	    if( non_contig_vecs ) {
		bool first = TRUE;
		while( rp != NULL ) {
		    char buf[20];
		    if( rp->upper == rp->lower ) {
			Sprintf(buf, "%d", rp->upper);
		    } else {
			Sprintf(buf, "%d:%d", rp->upper, rp->lower);
		    }
		    if( !first ) {
			charappend(',');
		    }
		    first = FALSE;
		    strappend(buf);
		    rp = rp->next;
		}
	    } else {
		string end = start_buf + strlen(start_buf);
		while( rp != NULL ) {
		    // Reset string
		    *end = 0;
		    strtemp(start_buf);
		    char buf[20];
		    if( rp->upper == rp->lower ) {
			Sprintf(buf, "%d", rp->upper);
		    } else {
			Sprintf(buf, "%d:%d", rp->upper, rp->lower);
		    }
		    strappend(buf);
		    emit_vector(vp->next, non_contig_vecs, tailp);
		    rp = rp->next;
		}
		return;
	    }
	}
	vp = vp->next;
    }
    SET_CONS_HD(*tailp, Make_STRING_leaf(wastrsave(&strings, start_buf)));
    SET_CONS_TL(*tailp, Make_NIL());
    *tailp = GET_CONS_TL(*tailp);
}

static bool
same_range(range_ptr r1, range_ptr r2)
{
    while (1) {
	if( r1 == r2 ) { return TRUE; }
	if( r1 == NULL ) { return FALSE; }
	if( r2 == NULL ) { return FALSE; }
	if( r1->upper != r2->upper ) { return FALSE; }
	if( r1->lower != r2->lower ) { return FALSE; }
	r1 = r1->next;
	r2 = r2->next;
    }
}

static range_ptr
compress_ranges(range_ptr r1, range_ptr r2)
{
    if( r1 == NULL ) return r2; 
    if( r2 == NULL ) return r1; 
    // Make sure r1 is the larger range
    if( r1->upper < r2->upper ) {
	range_ptr tmp = r2;
	r2 = r1;
	r1 = tmp;
    }
    if( r1->lower > (r2->upper+1) ) {
	// Non-overlapping
	r1->next = compress_ranges(r1->next, r2);
	return r1;
    }
    if( r1->lower <= r2->lower ) {
	// r1 covers r2. Just ignore r2
	return( compress_ranges(r1, r2->next) );
    }
    r1->lower = r2->lower;
    // Can we merge newly formed range with next range?
    if( r1->next != NULL ) {
	if( r1->lower == (r1->next->upper+1) ) {
	    r1->lower = r1->next->lower;
	    r1->next = r1->next->next;
	}
    }
    return( compress_ranges(r1, r2->next) );
}

static vec_ptr
split_name(string name)
{
    vec_ptr res = NULL;
    vec_ptr *res_tl_ptr = &res;
    string p = name;
    while( *p ) {
	if( isdigit(*p) ) {
	    // Index
	    string e = p;
	    while( *e && *e != ']' ) e++;
	    if( *e == 0 ) { goto illegal_format; }
	    char tmp = *e;
	    *e = 0;
	    range_ptr indices = NULL;
	    range_ptr *indices_tl_ptr = &indices;
	    while( *p ) {
		int upper, lower;
		if( sscanf(p, "%d:%d", &upper, &lower) == 2 ) {
		    range_ptr range = (range_ptr) new_rec(range_rec_mgrp);
		    range->upper = upper;
		    range->lower = lower;
		    range->next = NULL;
		    *indices_tl_ptr = range;
		    indices_tl_ptr = &(range->next);
		} else if( sscanf(p, "%d", &upper) == 1 ) {
		    range_ptr range = (range_ptr) new_rec(range_rec_mgrp);
		    range->upper = upper;
		    range->lower = upper;
		    range->next = NULL;
		    *indices_tl_ptr = range;
		    indices_tl_ptr = &(range->next);
		} else {
		    goto illegal_format;
		}
		while( *p && *p != ',' ) p++;
		if( *p ) p++;
	    }
	    vec_ptr n = (vec_ptr) new_rec(vec_rec_mgrp);
	    n->type = INDEX;
	    n->u.ranges = indices;
	    n->next = NULL;
	    *res_tl_ptr = n;
	    res_tl_ptr = &(n->next);
	    *e = tmp;
	    p = e;
	} else {
	    // String
	    if( *p == '\\' ) {
		// An escaped identifier
		string e = p+1;
		while( *e && *e != ' ' ) e++;
		if( *e == 0 ) { goto illegal_format; }
		e++;
		char tmp = *e;
		*e = 0;
		vec_ptr n = (vec_ptr) new_rec(vec_rec_mgrp);
		n->type = TXT;
		n->u.name = uStrsave(lstringsp, p);
		n->next = NULL;
		*res_tl_ptr = n;
		res_tl_ptr = &(n->next);
		*e = tmp;
		p = e;
	    } else {
		string e = p;
		while( *e && *e != '[' ) e++;
		if( *e == '[' ) e++;
		char tmp = *e;
		*e = 0;
		vec_ptr n = (vec_ptr) new_rec(vec_rec_mgrp);
		n->type = TXT;
		n->u.name = uStrsave(lstringsp, p);
		n->next = NULL;
		*res_tl_ptr = n;
		res_tl_ptr = &(n->next);
		*e = tmp;
		p = e;
	    }
	}
    }
    return res;

  illegal_format:
    res  = (vec_ptr) new_rec(vec_rec_mgrp);
    res->type = TXT;
    res->u.name = uStrsave(lstringsp, name);
    res->next = NULL;
    return res;
}

static sname_list_ptr *
prepend_index(int idx, sname_list_ptr rem, sname_list_ptr *resp)
{
    char buf[10];
    Sprintf(buf, "%d", idx);
    for(sname_list_ptr np = rem; np != NULL; np = np->next) {
	sname_list_ptr nnp = new_rec(sname_list_rec_mgrp);
	string nname = strtemp(buf);
	nname = strappend(np->name);
	nnp->name = uStrsave(lstringsp, nname);
	nnp->next = NULL;
	*resp = nnp;
	resp = &(nnp->next);
    }
    return resp;
}

static sname_list_ptr
expand_vec(vec_ptr vec)
{
    if( vec == NULL ) {
	sname_list_ptr np = new_rec(sname_list_rec_mgrp);
	np->next = NULL;
	np->name = uStrsave(lstringsp, "");
	return np;
    }
    sname_list_ptr rem = expand_vec(vec->next);
    if( vec->type == TXT ) {
	// A text item
	for(sname_list_ptr np = rem; np != NULL; np = np->next) {
	    string nname = strtemp(vec->u.name);
	    nname = strappend(np->name);
	    np->name = uStrsave(lstringsp, nname);
	}
	return rem;
    } else {
	// Indices
	sname_list_ptr res = NULL;
	sname_list_ptr *tailp = &res;
	for(range_ptr rp = vec->u.ranges; rp != NULL; rp = rp->next) {
	    if( rp->upper >= rp->lower ) {
		for(int i = rp->upper; i >= rp->lower; i--) {
		    tailp = prepend_index(i, rem, tailp);
		}
	    } else {
		for(int i = rp->upper; i <= rp->lower; i++) {
		    tailp = prepend_index(i, rem, tailp);
		}
	    }
	}
	return res;
    }
}

static int
vec_name_cmp(vec_ptr v1, vec_ptr v2)
{
    if( v1 == NULL ) {
	if( v2 == NULL ) {
	    return 0;
	} else {
	    return( -1 );
	}
    } else {
	if( v2 == NULL ) {
	    return( 1 );
	}
    }
    if( v1->type == TXT ) {
	if( v2->type == TXT ) {
	    if( v1->u.name == v2->u.name ) {
		return( vec_name_cmp(v1->next, v2->next) );
	    }
	    return( strcmp(v1->u.name, v2->u.name) );
	}
	return( 1 );
    }
    // v1->type == INDEX
    if( v2->type == TXT ) {
	if( v1->u.name == v2->u.name ) {
	    return( vec_name_cmp(v1->next, v2->next) );
	}
	return( strcmp(v1->u.name, v2->u.name) );
    }
    // v1->type == INDEX
    // v2->type == INDEX
    if( v1->u.ranges->upper == v2->u.ranges->upper ) {
	return( vec_name_cmp(v1->next, v2->next) );
    }
    if( v1->u.ranges->upper > v2->u.ranges->upper ) {
	return(1);
    } else {
	return(-1);
    }
}

static int
nn_cmp(const void *pi, const void *pj)
{
    vec_ptr vi = *((vec_ptr *) pi);
    vec_ptr vj = *((vec_ptr *) pj);
    return( vec_name_cmp(vi, vj) );
}

static void
gen_extract_vectors(g_ptr redex, bool non_contig_vecs)
{
    g_ptr l = GET_APPLY_LEFT(redex);
    g_ptr r = GET_APPLY_RIGHT(redex);
    if( IS_NIL(r) ) {
	MAKE_REDEX_NIL(redex);
	DEC_REF_CNT(l);
	DEC_REF_CNT(r);
	return;
    }
    // At least one vector in the list
    begin_vector_ops();
    buffer name_buf;
    new_buf(&name_buf, 100, sizeof(vec_ptr));
    for(g_ptr np = r; !IS_NIL(np); np = GET_CONS_TL(np)) {
	vec_ptr vp = split_name(GET_STRING(GET_CONS_HD(np)));
	for(vec_ptr cp = vp; cp != NULL; cp = cp->next) {
	    if( cp->type == INDEX ) {
		cp->u.ranges = make_range_canonical(cp->u.ranges); 
	    }
	}
	push_buf(&name_buf, &vp);
    }
    // Sort according to merge order
    qsort(START_BUF(&name_buf), COUNT_BUF(&name_buf), sizeof(vec_ptr), nn_cmp);
    vec_ptr *vpp;
    merge_list_ptr  mp = NULL;
    int alts = 0;
    FOR_BUF(&name_buf, vec_ptr, vpp) {
	merge_list_ptr m = new_rec (merge_list_rec_mgrp);
	m->vec = *vpp;
	m->name_signature = mk_name_signature(*vpp);
	if( mp == NULL ) {
	    m->next = m;
	    m->prev = m;
	    mp = m;
	} else {
	    m->next = mp;
	    m->prev = mp->prev;
	    mp->prev->next = m;
	    mp->prev = m;
	}
	alts++;
    }
    // Now try to merge adjacent vectors into bigger vectors
    int not_changed = 0;
    do {
	if( alts != 1 ) {
	    if( mp->name_signature != mp->next->name_signature ) {
		// Definitely cannot be merged
		not_changed++;
		mp = mp->next;
	    } else {
		// Potentially can be merged
		vec_ptr v1 = mp->vec;
		vec_ptr v2 = mp->next->vec;
		while( v1 &&
		      (v1->type == TXT ||
		       same_range(v1->u.ranges,v2->u.ranges)) )
		{
		    v1 = v1->next;
		    v2 = v2->next;
		}
		if( v1 == NULL ) {
		    // Two identical vectors
		    mp->next->next->prev = mp;
		    mp->next = mp->next->next;
		    not_changed = 0;
		    alts--;
		} else {
		    // Make sure the rest matches
		    range_ptr r1 = v1->u.ranges;
		    range_ptr r2 = v2->u.ranges;
		    vec_ptr mvp = v1;
		    v1 = v1->next;
		    v2 = v2->next;
		    while( v1 && 
			   (v1->type == TXT ||
			    same_range(v1->u.ranges,v2->u.ranges)) )
		    {
			v1 = v1->next;
			v2 = v2->next;
		    }
		    if( v1 != NULL ) {
			not_changed++;
			mp = mp->next;
		    } else {
			mvp->u.ranges = compress_ranges(r1, r2);
			mp->next->next->prev = mp;
			mp->next = mp->next->next;
			not_changed = 0;
			alts--;
		    }
		}
	    }
	}
    } while( alts > 1 && (not_changed <= alts) );
    MAKE_REDEX_NIL(redex);
    g_ptr tail = redex;
    for(int i = 0; i < alts; i++ ) {
	strtemp("");
	emit_vector(mp->vec, non_contig_vecs, &tail);
	mp = mp->next;
    }
    end_vector_ops();
    DEC_REF_CNT(l);
    DEC_REF_CNT(r);
}

static void
gen_merge_vectors(g_ptr redex, g_ptr r, bool non_contig_vecs)
{
    if( IS_NIL(r) ) {
	MAKE_REDEX_NIL(redex);
	return;
    }
    // At least one vector in the list
    begin_vector_ops();
    buffer name_buf;
    new_buf(&name_buf, 100, sizeof(vec_ptr));
    for(g_ptr np = r; !IS_NIL(np); np = GET_CONS_TL(np)) {
	vec_ptr vp = split_name(GET_STRING(GET_CONS_HD(np)));
	push_buf(&name_buf, &vp);
    }
    vec_ptr *vpp;
    merge_list_ptr  mp = NULL;
    int alts = 0;
    FOR_BUF(&name_buf, vec_ptr, vpp) {
	merge_list_ptr m = new_rec (merge_list_rec_mgrp);
	m->vec = *vpp;
	m->name_signature = mk_name_signature(*vpp);
	if( mp == NULL ) {
	    m->next = m;
	    m->prev = m;
	    mp = m;
	} else {
	    m->next = mp;
	    m->prev = mp->prev;
	    mp->prev->next = m;
	    mp->prev = m;
	}
	alts++;
    }
    // Now try to merge adjacent vectors into bigger vectors
    merge_list_ptr mp0 = mp;
    int not_changed = 0;
    while( alts > 1 && not_changed < alts ) {
	if( mp->next == mp0 ) {
	    // Never merge the last with the first
	    mp = mp0;
	}
	if( mp->name_signature != mp->next->name_signature ) {
	    // Definitely cannot be merged
	    not_changed++;
	    mp = mp->next;
	} else {
	    // Potentially can be merged
	    vec_ptr v1 = mp->vec;
	    vec_ptr v2 = mp->next->vec;
	    while( v1 &&
		  (v1->type == TXT ||
		   same_range(v1->u.ranges,v2->u.ranges)) )
	    {
		v1 = v1->next;
		v2 = v2->next;
	    }
	    if( v1 == NULL ) {
		// Two identical vectors; don't remove duplicates
		not_changed++;
		mp = mp->next;
	    } else {
		// Make sure the rest matches
		range_ptr r1 = v1->u.ranges;
		range_ptr r2 = v2->u.ranges;
		v1 = v1->next;
		v2 = v2->next;
		while( v1 && 
		       (v1->type == TXT ||
			same_range(v1->u.ranges,v2->u.ranges)) )
		{
		    v1 = v1->next;
		    v2 = v2->next;
		}
		if( v1 != NULL ) {
		    not_changed++;
		    mp = mp->next;
		} else {
		    if( (r1->upper >= r1->lower) && (r1->next == NULL) &&
			(r2->upper >= r2->lower) && (r2->next == NULL) &&
			(r2->upper == (r1->lower - 1)) )
		    {
			r1->lower = r2->lower;
			mp->next->next->prev = mp;
			mp->next = mp->next->next;
			not_changed = 0;
			alts--;
		    } else if( (r1->upper <= r1->lower) && (r1->next == NULL) &&
			       (r2->upper <= r2->lower) && (r2->next == NULL) &&
			       (r2->upper == (r1->lower + 1)) )
		    {
			r1->lower = r2->lower;
			mp->next->next->prev = mp;
			mp->next = mp->next->next;
			not_changed = 0;
			alts--;
		    } else {
			not_changed++;
			mp = mp->next;
		    }
		}
	    }
	}
    };
    MAKE_REDEX_NIL(redex);
    g_ptr tail = redex;
    mp = mp0;
    for(int i = 0; i < alts; i++ ) {
	strtemp("");
	emit_vector(mp->vec, non_contig_vecs, &tail);
	mp = mp->next;
    }
    end_vector_ops();
}

static string
mk_name_signature(vec_ptr vp)
{
    string name = strtemp("");
    while(vp != NULL ) {
        if( vp->type == TXT ) {
            name = strappend(vp->u.name);
        } else {
            name = charappend('*');
        }
        vp = vp->next;
    }
    return( uStrsave(lstringsp, name) );
}

static int
vec_size(vec_ptr vec)
{
    if( vec == NULL ) {
        return 1;
    }
    int rem = vec_size(vec->next);
    if( vec->type == TXT ) {
        return rem;
    } else {
        // Indices
        int sum = 0;
        for(range_ptr rp = vec->u.ranges; rp != NULL; rp = rp->next) {
            sum += abs(rp->upper-rp->lower+1)*rem;
        }
        return sum;
    }
}

