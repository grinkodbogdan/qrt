/* html.h - turns an HTML (or plain text) page into a flat list of styled
 * words, breaks and form fields for the browser to lay out.  No CSS, no
 * scripts, no images: the structure and the text of the page. */
#pragma once
#ifndef HTML_HOST_TEST
#include "../kernel/rt.h"
#endif

enum { N_TEXT, N_BREAK, N_GAP, N_HR, N_FIELD };
enum { HF_BOLD = 1, HF_MONO = 2, HF_PRE = 4, HF_BULLET = 8, HF_ALT = 16, HF_SMALL = 32 };
enum { FIELD_TEXT, FIELD_SUBMIT, FIELD_HIDDEN };

typedef struct {
    u8 kind, flags, heading, indent;   /* heading 1..6, 0 = body text; indent = nesting level */
    u8 space;                          /* N_TEXT: a space before it; N_GAP: size (1..3) */
    i32 link, field;                   /* -1 = none */
    u32 off, len;                      /* N_TEXT: bytes in doc.text */
} hnode_t;

typedef struct { int form, kind; char name[64], value[256]; } hfield_t;
typedef struct { char action[1024]; int post; } hform_t;

typedef struct {
    char *text; usize text_len, text_cap;
    hnode_t *nodes; int n_nodes, cap_nodes;
    char **links; int n_links, cap_links;
    hfield_t *fields; int n_fields, cap_fields;
    hform_t *forms; int n_forms, cap_forms;
    char title[160];
    char base[2300];                   /* for relative links */
} hdoc_t;

void html_parse(hdoc_t *d, const char *url, const u8 *src, usize len, int plain_text, int latin1);
void html_free(hdoc_t *d);
int  html_is_utf8(const u8 *s, usize len);
