#include "latex_unicode.h"
#include "util/buf.h"
#include "util/utf8.h"
#include <string.h>
#include <stdint.h>
#include <errno.h>

/* ---------------- command table ---------------- */

struct latex_cmd {
	const char *name;
	const char *uni;
	uint8_t nlen;
	uint8_t ulen;
	uint8_t arity;
};

#define CMD(n, u, a) { (n), (u), (uint8_t)(sizeof(n) - 1), (uint8_t)(sizeof(u) - 1), (a) }

static const struct latex_cmd cmd_table[] = {
CMD("Alpha","\xce\x91",0), CMD("Beta","\xce\x92",0),
CMD("Box","\xe2\x96\xa1",0), CMD("Bumpeq","\xe2\x89\x8e",0),
CMD("Chi","\xce\xa7",0),
CMD("Coloneqq","\xe2\xa9\xb5",0), CMD("Dagger","\xe2\x80\xa1",0),
CMD("Delta","\xce\x94",0), CMD("Diamond","\xe2\x97\x8a",0),
CMD("Downarrow","\xe2\x87\x93",0), CMD("Eqqcolon","\xe2\xa9\xb6",0),
CMD("Epsilon","\xce\x95",0), CMD("Eta","\xce\x97",0),
CMD("Gamma","\xce\x93",0), CMD("Im","\xe2\x84\x91",0),
CMD("Kappa","\xce\x9a",0), CMD("Lambda","\xce\x9b",0),
CMD("Leftarrow","\xe2\x87\x90",0),
CMD("Leftrightarrow","\xe2\x87\x94",0),
CMD("Longleftarrow","\xe2\x9f\xb8",0),
CMD("Longleftrightarrow","\xe2\x9f\xba",0),
CMD("Longrightarrow","\xe2\x9f\xb9",0),
CMD("Lsh","\xe2\x86\xb0",0), CMD("Mu","\xce\x9c",0),
CMD("Nu","\xce\x9d",0), CMD("Omega","\xce\xa9",0),
CMD("Omicron","\xce\x9f",0), CMD("Phi","\xce\xa6",0),
CMD("Pi","\xce\xa0",0), CMD("Psi","\xce\xa8",0),
CMD("Re","\xe2\x84\x9c",0), CMD("Rho","\xce\xa1",0),
CMD("Rightarrow","\xe2\x87\x92",0), CMD("Rsh","\xe2\x86\xb1",0),
CMD("Sigma","\xce\xa3",0), CMD("Theta","\xce\x98",0),
CMD("Updownarrow","\xe2\x87\x95",0), CMD("Uparrow","\xe2\x87\x91",0),
CMD("Upsilon","\xce\xa5",0), CMD("Vdash","\xe2\x8a\xa9",0),
CMD("VDash","\xe2\x8a\xab",0), CMD("Vert","\xe2\x80\x96",0),
CMD("Vvdash","\xe2\x8a\xaa",0), CMD("Xi","\xce\x9e",0),
CMD("Zeta","\xce\x96",0),
CMD("aleph","\xe2\x84\xb5",0), CMD("alpha","\xce\xb1",0),
CMD("amalg","\xe2\xa8\xbf",0), CMD("angle","\xe2\x88\xa0",0),
CMD("approx","\xe2\x89\x88",0), CMD("arccos","arccos",0),
CMD("arcsin","arcsin",0), CMD("arctan","arctan",0),
CMD("arg","arg",0), CMD("asymp","\xe2\x89\x8d",0),
CMD("ast","\xe2\x88\x97",0),
CMD("backepsilon","\xcf\xb6",0),
CMD("backprime","\xe2\x80\xb5",0),
CMD("backsim","\xe2\x88\xbd",0),
CMD("backslash","\xe2\x88\x96",0),
CMD("bar","\xcb\x86",0),
CMD("barwedge","\xe2\x8a\xbc",0),
CMD("because","\xe2\x88\xb5",0), CMD("beta","\xce\xb2",0),
CMD("beth","\xe2\x84\xb6",0),
CMD("bigcap","\xe2\x8b\x82",0), CMD("bigcup","\xe2\x8b\x83",0),
CMD("bigodot","\xe2\xa8\x80",0), CMD("bigoplus","\xe2\xa8\x81",0),
CMD("bigotimes","\xe2\xa8\x82",0),
CMD("bigsqcup","\xe2\xa8\x86",0),
CMD("bigtriangleup","\xe2\x96\xb3",0),
CMD("bigtriangledown","\xe2\x96\xbd",0),
CMD("biguplus","\xe2\xa8\x84",0),
CMD("bigvee","\xe2\x8b\x81",0), CMD("bigwedge","\xe2\x8b\x80",0),
CMD("binom","C(",2),
CMD("blacklozenge","\xe2\xa7\xab",0),
CMD("blacksquare","\xe2\x96\xa0",0),
CMD("blacktriangle","\xe2\x96\xb2",0),
CMD("blacktriangledown","\xe2\x96\xbc",0),
CMD("bot","\xe2\x8a\xa5",0), CMD("bowtie","\xe2\x8b\x88",0),
CMD("boxdot","\xe2\x8a\xa1",0),
CMD("boxminus","\xe2\x8a\x9f",0),
CMD("boxplus","\xe2\x8a\x9e",0),
CMD("boxtimes","\xe2\x8a\xa0",0),
CMD("breve","\xcb\x98",0),
CMD("bullet","\xe2\x88\x99",0),
CMD("bumpeq","\xe2\x89\x8f",0),
CMD("celsius","\xe2\x84\x83",0),
CMD("cdots","\xe2\x8b\xaf",0),
CMD("centerdot","\xe2\x8b\x85",0),
CMD("cfrac","",2),
CMD("check","\xcb\x87",0),
CMD("checkmark","\xe2\x9c\x93",0),
CMD("chi","\xcf\x87",0), CMD("circeq","\xe2\x89\x97",0),
CMD("circlearrowleft","\xe2\x86\xba",0),
CMD("circlearrowright","\xe2\x86\xbb",0),
CMD("circledast","\xe2\x8a\x9b",0),
CMD("circledcirc","\xe2\x8a\x9a",0),
CMD("circleddash","\xe2\x8a\x9d",0),
CMD("clubsuit","\xe2\x99\xa3",0),
CMD("colon",":",0),
CMD("coloneq","\xe2\x89\x94",0),
CMD("coloneqq","\xe2\xa9\xb4",0),
CMD("comma","",0),
CMD("complement","\xe2\x88\x81",0),
CMD("cong","\xe2\x89\x85",0),
CMD("coprod","\xe2\x88\x90",0), CMD("cos","cos",0),
CMD("cosh","cosh",0), CMD("cot","cot",0),
CMD("cup","\xe2\x88\xaa",0),
CMD("curlyeqprec","\xe2\x8b\x9e",0),
CMD("curlyeqsucc","\xe2\x8b\x9f",0),
CMD("curlyvee","\xe2\x8b\x8e",0),
CMD("curlywedge","\xe2\x8b\x8f",0),
CMD("curvearrowleft","\xe2\x86\xb6",0),
CMD("curvearrowright","\xe2\x86\xb7",0),
CMD("dagger","\xe2\x80\xa0",0),
CMD("daleth","\xe2\x84\xb8",0),
CMD("dashleftarrow","\xe2\x87\xa0",0),
CMD("dashrightarrow","\xe2\x87\xa2",0),
CMD("dblcolon","\xe2\x88\xb7",0),
CMD("ddagger","\xe2\x80\xa1",0),
CMD("ddot","\xc2\xa8",0),
CMD("ddots","\xe2\x8b\xb1",0), CMD("deg","deg",0),
CMD("delta","\xce\xb4",0),
CMD("det","det",0), CMD("diamond","\xe2\x8b\x84",0),
CMD("diamondsuit","\xe2\x99\xa2",0),
CMD("digamma","\xcf\x9d",0), CMD("dim","dim",0),
CMD("displaystyle","",0),
CMD("div","\xc3\xb7",0),
CMD("divideontimes","\xe2\x8b\x87",0),
CMD("dot","\xcb\x99",0),
CMD("doteq","\xe2\x89\x90",0),
CMD("doteqdot","\xe2\x89\x91",0),
CMD("dotplus","\xe2\x88\x94",0),
CMD("downarrow","\xe2\x86\x93",0),
CMD("downharpoonleft","\xe2\x87\x83",0),
CMD("downharpoonright","\xe2\x87\x82",0),
CMD("dfrac","",2),
CMD("ell","\xe2\x84\x93",0),
CMD("emptyset","\xe2\x88\x85",0),
CMD("enspace","  ",0),
CMD("epsilon","\xcf\xb5",0),
CMD("eqcirc","\xe2\x89\x96",0),
CMD("eqdef","\xe2\x89\x9d",0),
CMD("equiv","\xe2\x89\xa1",0),
CMD("eta","\xce\xb7",0), CMD("euro","\xe2\x82\xac",0),
CMD("excl","",0),
CMD("exists","\xe2\x88\x83",0), CMD("exp","exp",0),
CMD("fallingdotseq","\xe2\x89\x92",0),
CMD("flat","\xe2\x99\xad",0),
CMD("forall","\xe2\x88\x80",0),
CMD("frac","",2), CMD("frown","\xe2\x8c\xa2",0),
CMD("gamma","\xce\xb3",0), CMD("gcd","gcd",0),
CMD("ge","\xe2\x89\xa5",0), CMD("geq","\xe2\x89\xa5",0),
CMD("geqq","\xe2\x89\xa7",0),
CMD("geqslant","\xe2\xa9\xbe",0),
CMD("gets","\xe2\x86\x90",0), CMD("gg","\xe2\x89\xab",0),
CMD("ggg","\xe2\x8b\x99",0), CMD("gimel","\xe2\x84\xb7",0),
CMD("grave","`",0), CMD("gt",">",0),
CMD("hbar","\xe2\x84\x8f",0),
CMD("heartsuit","\xe2\x99\xa1",0),
CMD("hom","hom",0),
CMD("hookleftarrow","\xe2\x86\xa9",0),
CMD("hookrightarrow","\xe2\x86\xaa",0),
CMD("hspace","",1),
CMD("iff","\xe2\x9f\xba",0),
CMD("impliedby","\xe2\x9f\xb8",0),
CMD("implies","\xe2\x9f\xb9",0),
CMD("in","\xe2\x88\x88",0), CMD("inf","inf",0),
CMD("infty","\xe2\x88\x9e",0),
CMD("int","\xe2\x88\xab",0),
CMD("intclockwise","\xe2\x88\xb1",0),
CMD("intercal","\xe2\x8a\xba",0),
CMD("iint","\xe2\x88\xac",0),
CMD("iiint","\xe2\x88\xad",0),
CMD("iota","\xce\xb9",0),
CMD("jmath","\xc8\xb7",0),
CMD("kappa","\xce\xba",0), CMD("ker","ker",0),
CMD("lceil","\xe2\x8c\x88",0),
CMD("ldots","\xe2\x80\xa6",0),
CMD("le","\xe2\x89\xa4",0), CMD("left","",0),
CMD("leftarrow","\xe2\x86\x90",0),
CMD("leftarrowtail","\xe2\x86\xa2",0),
CMD("leftharpoondown","\xe2\x86\xbd",0),
CMD("leftharpoonup","\xe2\x86\xbc",0),
CMD("leftrightarrow","\xe2\x86\x94",0),
CMD("leftrightharpoons","\xe2\x87\x8b",0),
CMD("leftrightsquigarrow","\xe2\x86\xad",0),
CMD("leftthreetimes","\xe2\x8b\x8b",0),
CMD("leq","\xe2\x89\xa4",0),
CMD("leqq","\xe2\x89\xa6",0),
CMD("leqslant","\xe2\xa9\xbd",0),
CMD("lfloor","\xe2\x8c\x8a",0),
CMD("lhd","\xe2\x8a\xb2",0),
CMD("lim","lim",0), CMD("liminf","lim inf",0),
CMD("limsup","lim sup",0),
CMD("ll","\xe2\x89\xaa",0),
CMD("llangle","\xe2\x9f\xaa",0),
CMD("llbracket","\xe2\x9f\xa6",0),
CMD("lll","\xe2\x8b\x98",0),
CMD("ln","ln",0),
CMD("lnapprox","\xe2\xaa\xb9",0),
CMD("lneq","\xe2\xaa\xb7",0),
CMD("lneqq","\xe2\x89\xa8",0),
CMD("log","log",0),
CMD("longleftarrow","\xe2\x9f\xb5",0),
CMD("longleftrightarrow","\xe2\x9f\xb7",0),
CMD("longmapsto","\xe2\x9f\xbc",0),
CMD("longrightarrow","\xe2\x9f\xb6",0),
CMD("looparrowleft","\xe2\x86\xab",0),
CMD("looparrowright","\xe2\x86\xac",0),
CMD("lor","\xe2\x88\xa8",0), CMD("lt","<",0),
CMD("ltimes","\xe2\x8b\x89",0),
CMD("maltese","\xe2\x9c\xa0",0),
CMD("mapsto","\xe2\x86\xa6",0),
CMD("mathbb","",1), CMD("mathbf","",1),
CMD("mathcal","",1), CMD("mathfrak","",1),
CMD("mathit","",1), CMD("mathrm","",1),
CMD("mathscr","",1), CMD("mathsf","",1),
CMD("mathtt","",1),
CMD("max","max",0),
CMD("measeq","\xe2\x89\x9e",0),
CMD("measuredangle","\xe2\x88\xa1",0),
CMD("micro","\xc2\xb5",0), CMD("min","min",0),
CMD("models","\xe2\x8a\xa8",0),
CMD("mp","\xe2\x88\x93",0), CMD("mu","\xce\xbc",0),
CMD("multimap","\xe2\x8a\xb8",0),
CMD("nabla","\xe2\x88\x87",0),
CMD("natural","\xe2\x99\xae",0),
CMD("ncong","\xe2\x89\x87",0), CMD("ne","\xe2\x89\xa0",0),
CMD("nearrow","\xe2\x86\x97",0),
CMD("neg","\xc2\xac",0), CMD("neq","\xe2\x89\xa0",0),
CMD("nequiv","\xe2\x89\xa2",0),
CMD("nexists","\xe2\x88\x84",0),
CMD("ngeq","\xe2\x89\xb1",0), CMD("ngtr","\xe2\x89\xaf",0),
CMD("ni","\xe2\x88\x8b",0),
CMD("nleftarrow","\xe2\x86\x9a",0),
CMD("nleftrightarrow","\xe2\x86\xae",0),
CMD("nleq","\xe2\x89\xb0",0), CMD("nless","\xe2\x89\xae",0),
CMD("nmid","\xe2\x88\xa4",0),
CMD("nolimits","",0),
CMD("not","\xcc\xb8",0),
CMD("notin","\xe2\x88\x89",0),
CMD("notni","\xe2\x88\x8c",0),
CMD("nparallel","\xe2\x88\xa6",0),
CMD("nprec","\xe2\x8a\x80",0),
CMD("nRightarrow","\xe2\x87\x8f",0),
CMD("nrightarrow","\xe2\x86\x9b",0),
CMD("nLeftarrow","\xe2\x87\x8d",0),
CMD("nLeftrightarrow","\xe2\x87\x8e",0),
CMD("nsim","\xe2\x89\x81",0),
CMD("nsucc","\xe2\x8a\x81",0),
CMD("nsubseteq","\xe2\x8a\x88",0),
CMD("nsupseteq","\xe2\x8a\x89",0),
CMD("nu","\xce\xbd",0),
CMD("nvDash","\xe2\x8a\xad",0),
CMD("nvdash","\xe2\x8a\xac",0),
CMD("nVDash","\xe2\x8a\xaf",0),
CMD("nVdash","\xe2\x8a\xae",0),
CMD("nwarrow","\xe2\x86\x96",0),
CMD("odot","\xe2\x8a\x99",0), CMD("ohm","\xce\xa9",0),
CMD("oint","\xe2\x88\xae",0),
CMD("oiint","\xe2\x88\xaf",0),
CMD("oiiint","\xe2\x88\xb0",0),
CMD("omega","\xcf\x89",0), CMD("omicron","\xce\xbf",0),
CMD("ominus","\xe2\x8a\x96",0),
CMD("oplus","\xe2\x8a\x95",0),
CMD("oslash","\xe2\x8a\x98",0),
CMD("otimes","\xe2\x8a\x97",0),
CMD("overbrace","",1),
CMD("overleftarrow","\xe2\x83\x96",0),
CMD("overline","",1),
CMD("overrightarrow","\xe2\x83\x97",0),
CMD("overset","",2),
CMD("parallel","\xe2\x88\xa5",0),
CMD("partial","\xe2\x88\x82",0),
CMD("perp","\xe2\x8a\xa5",0),
CMD("phi","\xcf\x95",0), CMD("pi","\xcf\x80",0),
CMD("pm","\xc2\xb1",0),
CMD("pounds","\xc2\xa3",0),
CMD("Pr","Pr",0),
CMD("prec","\xe2\x89\xba",0),
CMD("precapprox","\xe2\xaa\xb7",0),
CMD("preceq","\xe2\xaa\xaf",0),
CMD("precnapprox","\xe2\xaa\xb9",0),
CMD("precneqq","\xe2\xaa\xb5",0),
CMD("precnsim","\xe2\x8b\xa8",0),
CMD("precsim","\xe2\x89\xbe",0),
CMD("prime","\xe2\x80\xb2",0),
CMD("prod","\xe2\x88\x8f",0),
CMD("propto","\xe2\x88\x9d",0),
CMD("psi","\xcf\x88",0),
CMD("qquad","        ",0), CMD("quad","    ",0),
CMD("quest","",0),
CMD("questeq","\xe2\x89\x9f",0),
CMD("rangle","\xe2\x9f\xa9",0),
CMD("rbrack","]",0),
CMD("rceil","\xe2\x8c\x89",0),
CMD("rfloor","\xe2\x8c\x8b",0),
CMD("rho","\xcf\x81",0),
CMD("right","",0),
CMD("rightarrow","\xe2\x86\x92",0),
CMD("rightarrowtail","\xe2\x86\xa3",0),
CMD("rightharpoondown","\xe2\x87\x81",0),
CMD("rightharpoonup","\xe2\x87\x80",0),
CMD("rightleftharpoons","\xe2\x87\x8c",0),
CMD("rightthreetimes","\xe2\x8b\x8c",0),
CMD("risingdotseq","\xe2\x89\x93",0),
CMD("rhd","\xe2\x8a\xb3",0),
CMD("root","",2),
CMD("rtimes","\xe2\x8b\x8a",0),
CMD("searrow","\xe2\x86\x98",0),
CMD("sec","sec",0),
CMD("semicolon","",0),
CMD("setminus","\xe2\x88\x96",0),
CMD("sharp","\xe2\x99\xaf",0),
CMD("sigma","\xcf\x83",0), CMD("sim","\xe2\x88\xbc",0),
CMD("simeq","\xe2\x89\x83",0), CMD("sin","sin",0),
CMD("sinh","sinh",0),
CMD("slash","\xe2\x88\x95",0),
CMD("smallsetminus","\xe2\x88\x96",0),
CMD("smile","\xe2\x8c\xa3",0),
CMD("spadesuit","\xe2\x99\xa0",0),
CMD("sphericalangle","\xe2\x88\xa2",0),
CMD("sqcap","\xe2\x8a\x93",0),
CMD("sqcup","\xe2\x8a\x94",0),
CMD("sqsubset","\xe2\x8a\x8f",0),
CMD("sqsubseteq","\xe2\x8a\x91",0),
CMD("sqsupset","\xe2\x8a\x90",0),
CMD("sqsupseteq","\xe2\x8a\x92",0),
CMD("square","\xe2\x96\xa1",0),
CMD("stackrel","",2), CMD("star","\xe2\x8b\x86",0),
CMD("subset","\xe2\x8a\x82",0),
CMD("subseteq","\xe2\x8a\x86",0),
CMD("subsetneq","\xe2\x8a\x8a",0),
CMD("subsetneqq","\xe2\xab\x8b",0),
CMD("succ","\xe2\x89\xbb",0),
CMD("succapprox","\xe2\xaa\xb8",0),
CMD("succcurlyeq","\xe2\x89\xbd",0),
CMD("succeq","\xe2\xaa\xb0",0),
CMD("succnapprox","\xe2\xaa\xba",0),
CMD("succneqq","\xe2\xaa\xb6",0),
CMD("succnsim","\xe2\x8b\xa9",0),
CMD("succsim","\xe2\x89\xbf",0),
CMD("sum","\xe2\x88\x91",0), CMD("sup","sup",0),
CMD("supset","\xe2\x8a\x83",0),
CMD("supseteq","\xe2\x8a\x87",0),
CMD("supsetneq","\xe2\x8a\x8b",0),
CMD("supsetneqq","\xe2\xab\x8c",0),
CMD("surd","\xe2\x88\x9a",0),
CMD("swarrow","\xe2\x86\x99",0),
CMD("tan","tan",0), CMD("tanh","tanh",0),
CMD("tau","\xcf\x84",0),
CMD("textstyle","",0),
CMD("therefore","\xe2\x88\xb4",0),
CMD("theta","\xce\xb8",0),
CMD("thicksim","\xe2\x88\xbc",0),
CMD("tilde","\xcb\x9c",0),
CMD("times","\xc3\x97",0),
CMD("to","\xe2\x86\x92",0), CMD("top","\xe2\x8a\xa4",0),
CMD("triangle","\xe2\x96\xb3",0),
CMD("triangleleft","\xe2\x97\x83",0),
CMD("triangleq","\xe2\x89\x9c",0),
CMD("triangleright","\xe2\x96\xb9",0),
CMD("tfrac","",2),
CMD("underbrace","",1),
CMD("underline","",1),
CMD("underset","",2),
CMD("unlhd","\xe2\x8a\xb4",0),
CMD("unrhd","\xe2\x8a\xb5",0),
CMD("uparrow","\xe2\x86\x91",0),
CMD("updownarrow","\xe2\x86\x95",0),
CMD("upharpoonleft","\xe2\x86\xbf",0),
CMD("upharpoonright","\xe2\x86\xbe",0),
CMD("uplus","\xe2\x8a\x8e",0),
CMD("upsilon","\xcf\x85",0),
CMD("varepsilon","\xce\xb5",0),
CMD("varkappa","\xcf\xb0",0),
CMD("varnothing","\xe2\x88\x85",0),
CMD("varointclockwise","\xe2\x88\xb2",0),
CMD("varphi","\xcf\x86",0),
CMD("varpi","\xcf\x96",0),
CMD("varrho","\xcf\xb1",0),
CMD("varsigma","\xcf\x82",0),
CMD("vartheta","\xcf\x91",0),
CMD("vartriangle","\xe2\x96\xb3",0),
CMD("vdash","\xe2\x8a\xa2",0),
CMD("vdots","\xe2\x8b\xae",0),
CMD("vec","\xe2\x83\x97",0),
CMD("vee","\xe2\x88\xa8",0),
CMD("veebar","\xe2\x8a\xbb",0),
CMD("vert","|",0),
CMD("wedge","\xe2\x88\xa7",0),
CMD("widehat","",1),
CMD("widetilde","",1),
CMD("wp","\xe2\x84\x98",0),
CMD("wr","\xe2\x89\x80",0),
CMD("xi","\xce\xbe",0), CMD("zeta","\xce\xb6",0),
};
#define CMD_TABLE_SIZE (sizeof(cmd_table) / sizeof(cmd_table[0]))

/* ---------------- superscript / subscript ---------------- */

static const char *sup_d[] = {
"\xe2\x81\xb0","\xc2\xb9","\xc2\xb2","\xc2\xb3",
"\xe2\x81\xb4","\xe2\x81\xb5","\xe2\x81\xb6",
"\xe2\x81\xb7","\xe2\x81\xb8","\xe2\x81\xb9",
};
static const size_t sup_dl[] = {3,2,2,2,3,3,3,3,3,3};

static const char *sub_d[] = {
"\xe2\x82\x80","\xe2\x82\x81","\xe2\x82\x82","\xe2\x82\x83",
"\xe2\x82\x84","\xe2\x82\x85","\xe2\x82\x86",
"\xe2\x82\x87","\xe2\x82\x88","\xe2\x82\x89",
};
static const size_t sub_dl[] = {3,3,3,3,3,3,3,3,3,3};

struct ss_letter {
	char ch;
	const char *uni;
	uint8_t ulen;
};
#define SSL(c,u) { (c), (u), (uint8_t)(sizeof(u)-1) }

static const struct ss_letter sup_l[] = {
SSL('a',"\xe1\xb5\x83"),SSL('b',"\xe1\xb5\x87"),SSL('c',"\xe1\xb6\x9c"),
SSL('d',"\xe1\xb5\x88"),SSL('e',"\xe1\xb5\x89"),SSL('f',"\xe1\xb6\xa0"),
SSL('g',"\xe1\xb5\x8d"),SSL('h',"\xca\xb0"),SSL('i',"\xe2\x81\xb1"),
SSL('j',"\xca\xb2"),SSL('k',"\xe1\xb5\x8f"),SSL('l',"\xcb\xa1"),
SSL('m',"\xe1\xb5\x90"),SSL('n',"\xe2\x81\xbf"),SSL('o',"\xe1\xb5\x92"),
SSL('p',"\xe1\xb5\x96"),SSL('r',"\xca\xb3"),SSL('s',"\xcb\xa2"),
SSL('t',"\xe1\xb5\x97"),SSL('u',"\xe1\xb5\x98"),SSL('v',"\xe1\xb5\x9b"),
SSL('w',"\xca\xb7"),SSL('x',"\xcb\xa3"),SSL('y',"\xca\xb8"),
SSL('z',"\xe1\xb6\xbb"),
SSL('A',"\xe1\xb4\xac"),SSL('B',"\xe1\xb4\xae"),SSL('D',"\xe1\xb4\xb0"),
SSL('E',"\xe1\xb4\xb1"),SSL('G',"\xe1\xb4\xb3"),SSL('H',"\xe1\xb4\xb4"),
SSL('I',"\xe1\xb4\xb5"),SSL('K',"\xe1\xb4\xb7"),SSL('L',"\xe1\xb4\xb8"),
SSL('M',"\xe1\xb4\xb9"),SSL('N',"\xe1\xb4\xba"),SSL('O',"\xe1\xb4\xbc"),
SSL('P',"\xe1\xb4\xbe"),SSL('R',"\xe1\xb4\xbf"),SSL('T',"\xe1\xb5\x80"),
SSL('U',"\xe1\xb5\x81"),SSL('V',"\xe1\xb5\x82"),
};
#define SUP_L_SIZE (sizeof(sup_l)/sizeof(sup_l[0]))

static const struct ss_letter sub_l[] = {
SSL('a',"\xe2\x82\x90"),SSL('e',"\xe2\x82\x91"),SSL('h',"\xe2\x82\x95"),
SSL('i',"\xe1\xb5\xa2"),SSL('j',"\xe2\xb1\xbc"),SSL('k',"\xe2\x82\x96"),
SSL('l',"\xe2\x82\x97"),SSL('m',"\xe2\x82\x98"),SSL('n',"\xe2\x82\x99"),
SSL('o',"\xe2\x82\x92"),SSL('p',"\xe2\x82\x9a"),SSL('r',"\xe1\xb5\xa3"),
SSL('s',"\xe2\x82\x9b"),SSL('t',"\xe2\x82\x9c"),SSL('u',"\xe1\xb5\xa4"),
SSL('v',"\xe1\xb5\xa5"),SSL('x',"\xe2\x82\x93"),
};
#define SUB_L_SIZE (sizeof(sub_l)/sizeof(sub_l[0]))

/* ---------------- mathbb table ---------------- */

struct font_map {
	char ch;
	const char *uni;
	uint8_t ulen;
};
#define FM(c,u) { (c), (u), (uint8_t)(sizeof(u)-1) }

static const struct font_map bb_map[] = {
FM('A',"\xf0\x9d\x94\xb8"),FM('B',"\xf0\x9d\x94\xb9"),
FM('C',"\xe2\x84\x82"),FM('D',"\xf0\x9d\x94\xbb"),
FM('E',"\xf0\x9d\x94\xbc"),FM('F',"\xf0\x9d\x94\xbd"),
FM('G',"\xf0\x9d\x94\xbe"),FM('H',"\xe2\x84\x8d"),
FM('I',"\xf0\x9d\x95\x80"),FM('J',"\xf0\x9d\x95\x81"),
FM('K',"\xf0\x9d\x95\x82"),FM('L',"\xf0\x9d\x95\x83"),
FM('M',"\xf0\x9d\x95\x84"),FM('N',"\xe2\x84\x95"),
FM('O',"\xf0\x9d\x95\x86"),FM('P',"\xe2\x84\x99"),
FM('Q',"\xe2\x84\x9a"),FM('R',"\xe2\x84\x9d"),
FM('S',"\xf0\x9d\x95\x8a"),FM('T',"\xf0\x9d\x95\x8b"),
FM('U',"\xf0\x9d\x95\x8c"),FM('V',"\xf0\x9d\x95\x8d"),
FM('W',"\xf0\x9d\x95\x8e"),FM('X',"\xf0\x9d\x95\x8f"),
FM('Y',"\xf0\x9d\x95\x90"),FM('Z',"\xe2\x84\xa4"),
FM('0',"\xf0\x9d\x9f\x98"),FM('1',"\xf0\x9d\x9f\x99"),
FM('2',"\xf0\x9d\x9f\x9a"),FM('3',"\xf0\x9d\x9f\x9b"),
FM('4',"\xf0\x9d\x9f\x9c"),FM('5',"\xf0\x9d\x9f\x9d"),
FM('6',"\xf0\x9d\x9f\x9e"),FM('7',"\xf0\x9d\x9f\x9f"),
FM('8',"\xf0\x9d\x9f\xa0"),FM('9',"\xf0\x9d\x9f\xa1"),
};
#define BB_SIZE (sizeof(bb_map)/sizeof(bb_map[0]))

static const struct font_map cal_map[] = {
FM('A',"\xf0\x9d\x92\x9c"),FM('B',"\xe2\x84\xac"),
FM('C',"\xf0\x9d\x92\x9e"),FM('D',"\xf0\x9d\x92\x9f"),
FM('E',"\xe2\x84\xb0"),FM('F',"\xe2\x84\xb1"),
FM('G',"\xf0\x9d\x92\xa2"),FM('H',"\xe2\x84\x8b"),
FM('I',"\xe2\x84\x90"),FM('J',"\xf0\x9d\x92\xa5"),
FM('K',"\xf0\x9d\x92\xa6"),FM('L',"\xe2\x84\x92"),
FM('M',"\xe2\x84\xb3"),FM('N',"\xe2\x84\xb4"),
FM('O',"\xf0\x9d\x92\xa9"),FM('P',"\xf0\x9d\x92\xaa"),
FM('Q',"\xf0\x9d\x92\xab"),FM('R',"\xe2\x84\x9b"),
FM('S',"\xf0\x9d\x92\xae"),FM('T',"\xf0\x9d\x92\xaf"),
FM('U',"\xf0\x9d\x92\xb0"),FM('V',"\xf0\x9d\x92\xb1"),
FM('W',"\xf0\x9d\x92\xb2"),FM('X',"\xf0\x9d\x92\xb3"),
FM('Y',"\xf0\x9d\x92\xb4"),FM('Z',"\xf0\x9d\x92\xb5"),
};
#define CAL_SIZE (sizeof(cal_map)/sizeof(cal_map[0]))

static const struct font_map frk_map[] = {
FM('A',"\xf0\x9d\x94\x84"),FM('B',"\xf0\x9d\x94\x85"),
FM('C',"\xe2\x84\xad"),FM('D',"\xf0\x9d\x94\x87"),
FM('E',"\xf0\x9d\x94\x88"),FM('F',"\xf0\x9d\x94\x89"),
FM('G',"\xf0\x9d\x94\x8a"),FM('H',"\xe2\x84\x8c"),
FM('I',"\xe2\x84\x91"),FM('J',"\xf0\x9d\x94\x8d"),
FM('K',"\xf0\x9d\x94\x8e"),FM('L',"\xe2\x84\x92"),
FM('M',"\xe2\x84\xb3"),FM('N',"\xf0\x9d\x94\x90"),
FM('O',"\xf0\x9d\x94\x91"),FM('P',"\xf0\x9d\x94\x92"),
FM('Q',"\xf0\x9d\x94\x93"),FM('R',"\xe2\x84\x9c"),
FM('S',"\xf0\x9d\x94\x96"),FM('T',"\xf0\x9d\x94\x97"),
FM('U',"\xf0\x9d\x94\x98"),FM('V',"\xf0\x9d\x94\x99"),
FM('W',"\xf0\x9d\x94\x9a"),FM('X',"\xf0\x9d\x94\x9b"),
FM('Y',"\xf0\x9d\x94\x9c"),FM('Z',"\xe2\x84\xa8"),
FM('a',"\xf0\x9d\x94\x9e"),FM('b',"\xf0\x9d\x94\x9f"),
FM('c',"\xf0\x9d\x94\xa0"),FM('d',"\xf0\x9d\x94\xa1"),
FM('e',"\xf0\x9d\x94\xa2"),FM('f',"\xf0\x9d\x94\xa3"),
FM('g',"\xf0\x9d\x94\xa4"),FM('h',"\xf0\x9d\x94\xa5"),
FM('i',"\xf0\x9d\x94\xa6"),FM('j',"\xf0\x9d\x94\xa7"),
FM('k',"\xf0\x9d\x94\xa8"),FM('l',"\xf0\x9d\x94\xa9"),
FM('m',"\xf0\x9d\x94\xaa"),FM('n',"\xf0\x9d\x94\xab"),
FM('o',"\xf0\x9d\x94\xac"),FM('p',"\xf0\x9d\x94\xad"),
FM('q',"\xf0\x9d\x94\xae"),FM('r',"\xf0\x9d\x94\xaf"),
FM('s',"\xf0\x9d\x94\xb0"),FM('t',"\xf0\x9d\x94\xb1"),
FM('u',"\xf0\x9d\x94\xb2"),FM('v',"\xf0\x9d\x94\xb3"),
FM('w',"\xf0\x9d\x94\xb4"),FM('x',"\xf0\x9d\x94\xb5"),
FM('y',"\xf0\x9d\x94\xb6"),FM('z',"\xf0\x9d\x94\xb7"),
};
#define FRK_SIZE (sizeof(frk_map)/sizeof(frk_map[0]))

/* mathbf: A-Z → U+1D400+, a-z → U+1D41A+, 0-9 → U+1D7CE+ */
static const struct font_map bf_map[] = {
FM('A',"\xf0\x9d\x90\x80"),FM('B',"\xf0\x9d\x90\x81"),
FM('C',"\xf0\x9d\x90\x82"),FM('D',"\xf0\x9d\x90\x83"),
FM('E',"\xf0\x9d\x90\x84"),FM('F',"\xf0\x9d\x90\x85"),
FM('G',"\xf0\x9d\x90\x86"),FM('H',"\xf0\x9d\x90\x87"),
FM('I',"\xf0\x9d\x90\x88"),FM('J',"\xf0\x9d\x90\x89"),
FM('K',"\xf0\x9d\x90\x8a"),FM('L',"\xf0\x9d\x90\x8b"),
FM('M',"\xf0\x9d\x90\x8c"),FM('N',"\xf0\x9d\x90\x8d"),
FM('O',"\xf0\x9d\x90\x8e"),FM('P',"\xf0\x9d\x90\x8f"),
FM('Q',"\xf0\x9d\x90\x90"),FM('R',"\xf0\x9d\x90\x91"),
FM('S',"\xf0\x9d\x90\x92"),FM('T',"\xf0\x9d\x90\x93"),
FM('U',"\xf0\x9d\x90\x94"),FM('V',"\xf0\x9d\x90\x95"),
FM('W',"\xf0\x9d\x90\x96"),FM('X',"\xf0\x9d\x90\x97"),
FM('Y',"\xf0\x9d\x90\x98"),FM('Z',"\xf0\x9d\x90\x99"),
FM('a',"\xf0\x9d\x90\x9a"),FM('b',"\xf0\x9d\x90\x9b"),
FM('c',"\xf0\x9d\x90\x9c"),FM('d',"\xf0\x9d\x90\x9d"),
FM('e',"\xf0\x9d\x90\x9e"),FM('f',"\xf0\x9d\x90\x9f"),
FM('g',"\xf0\x9d\x90\xa0"),FM('h',"\xf0\x9d\x90\xa1"),
FM('i',"\xf0\x9d\x90\xa2"),FM('j',"\xf0\x9d\x90\xa3"),
FM('k',"\xf0\x9d\x90\xa4"),FM('l',"\xf0\x9d\x90\xa5"),
FM('m',"\xf0\x9d\x90\xa6"),FM('n',"\xf0\x9d\x90\xa7"),
FM('o',"\xf0\x9d\x90\xa8"),FM('p',"\xf0\x9d\x90\xa9"),
FM('q',"\xf0\x9d\x90\xaa"),FM('r',"\xf0\x9d\x90\xab"),
FM('s',"\xf0\x9d\x90\xac"),FM('t',"\xf0\x9d\x90\xad"),
FM('u',"\xf0\x9d\x90\xae"),FM('v',"\xf0\x9d\x90\xaf"),
FM('w',"\xf0\x9d\x90\xb0"),FM('x',"\xf0\x9d\x90\xb1"),
FM('y',"\xf0\x9d\x90\xb2"),FM('z',"\xf0\x9d\x90\xb3"),
FM('0',"\xf0\x9d\x9f\x8e"),FM('1',"\xf0\x9d\x9f\x8f"),
FM('2',"\xf0\x9d\x9f\x90"),FM('3',"\xf0\x9d\x9f\x91"),
FM('4',"\xf0\x9d\x9f\x92"),FM('5',"\xf0\x9d\x9f\x93"),
FM('6',"\xf0\x9d\x9f\x94"),FM('7',"\xf0\x9d\x9f\x95"),
FM('8',"\xf0\x9d\x9f\x96"),FM('9',"\xf0\x9d\x9f\x97"),
};
#define BF_SIZE (sizeof(bf_map)/sizeof(bf_map[0]))

/* ---------------- helper: font char lookup ---------------- */

static const char *font_lookup(char ch, const struct font_map *m, size_t cnt,
			       size_t *olen)
{
	for (size_t i = 0; i < cnt; i++) {
		if (m[i].ch == ch) {
			*olen = m[i].ulen;
			return m[i].uni;
		}
	}
	return NULL;
}

static void render_font_group(const char *s, size_t len, morph_buf_t *o,
			      const struct font_map *m, size_t cnt)
{
	for (size_t i = 0; i < len; ) {
		unsigned char c = (unsigned char)s[i];
		size_t olen = 0;
		const char *u;
		if (c < 0x80) {
			u = font_lookup((char)c, m, cnt, &olen);
			if (u) {
				morph_buf_append(o, u, olen);
			} else {
				morph_buf_putc(o, (char)c);
			}
			i++;
		} else if (c >= 0x80) {
			size_t b = utf8_next_codepoint_len(s + i, len - i);
			morph_buf_append(o, s + i, b);
			i += b;
		} else {
			i++;
		}
	}
}

/* ---------------- brace group helpers ---------------- */

static size_t skip_brace(const char *s, size_t len)
{
	if (len < 1 || s[0] != '{')
		return 0;
	size_t i = 1;
	int depth = 1;
	while (i < len && depth > 0) {
		if (s[i] == '\\' && i + 1 < len) {
			i += 2;
			continue;
		}
		if (s[i] == '{') depth++;
		else if (s[i] == '}') depth--;
		i++;
	}
	return i;
}

static size_t brace_content(const char *s, size_t len,
			    const char **start, size_t *clen)
{
	if (len < 1 || s[0] != '{') {
		*start = s;
		*clen = 0;
		return 0;
	}
	size_t i = 1;
	int depth = 1;
	while (i < len && depth > 0) {
		if (s[i] == '\\' && i + 1 < len) {
			i += 2;
			continue;
		}
		if (s[i] == '{') depth++;
		else if (s[i] == '}') depth--;
		i++;
	}
	*start = s + 1;
	*clen = i >= 2 ? i - 2 : 0;
	return i;
}

/* ---------------- superscript / subscript render ---------------- */

static int render_sup_char(char c, morph_buf_t *o)
{
	if (c >= '0' && c <= '9') {
		morph_buf_append(o, sup_d[(unsigned)(c - '0')], sup_dl[(unsigned)(c - '0')]);
		return 1;
	}
	if (c == '+') { morph_buf_puts(o, "\xe2\x81\xba"); return 1; }
	if (c == '-') { morph_buf_puts(o, "\xe2\x81\xbb"); return 1; }
	if (c == '=') { morph_buf_puts(o, "\xe2\x81\xbc"); return 1; }
	if (c == '(') { morph_buf_puts(o, "\xe2\x81\xbd"); return 1; }
	if (c == ')') { morph_buf_puts(o, "\xe2\x81\xbe"); return 1; }
	if (c == 'n') { morph_buf_puts(o, "\xe2\x81\xbf"); return 1; }
	if (c == 'i') { morph_buf_puts(o, "\xe2\x81\xb1"); return 1; }
	for (size_t k = 0; k < SUP_L_SIZE; k++) {
		if (sup_l[k].ch == c) {
			morph_buf_append(o, sup_l[k].uni, sup_l[k].ulen);
			return 1;
		}
	}
	return 0;
}

static int render_sub_char(char c, morph_buf_t *o)
{
	if (c >= '0' && c <= '9') {
		morph_buf_append(o, sub_d[(unsigned)(c - '0')], sub_dl[(unsigned)(c - '0')]);
		return 1;
	}
	if (c == '+') { morph_buf_puts(o, "\xe2\x82\x8a"); return 1; }
	if (c == '-') { morph_buf_puts(o, "\xe2\x82\x8b"); return 1; }
	if (c == '=') { morph_buf_puts(o, "\xe2\x82\x8c"); return 1; }
	if (c == '(') { morph_buf_puts(o, "\xe2\x82\x8d"); return 1; }
	if (c == ')') { morph_buf_puts(o, "\xe2\x82\x8e"); return 1; }
	for (size_t k = 0; k < SUB_L_SIZE; k++) {
		if (sub_l[k].ch == c) {
			morph_buf_append(o, sub_l[k].uni, sub_l[k].ulen);
			return 1;
		}
	}
	return 0;
}

/* forward decl */
static size_t render_expr(const char *s, size_t len, morph_buf_t *o);

static size_t render_sup(morph_buf_t *o, const char *s, size_t len)
{
	if (len == 0)
		return 0;
	if (s[0] == '{') {
		const char *c;
		size_t cl;
		size_t skip = brace_content(s, len, &c, &cl);
		if (skip == 0)
			return 0;
		/* single char in braces: try unicode */
		if (cl == 1 && render_sup_char(c[0], o))
			return skip;
		/* multi-char: fallback ^(expr) */
		morph_buf_putc(o, '^');
		morph_buf_putc(o, '(');
		render_expr(c, cl, o);
		morph_buf_putc(o, ')');
		return skip;
	}
	if (render_sup_char(s[0], o))
		return 1;
	morph_buf_putc(o, '^');
	{
		size_t b = utf8_next_codepoint_len(s, len);
		if (b == 0)
			b = 1;
		morph_buf_append(o, s, b);
		return b;
	}
}

static size_t render_sub(morph_buf_t *o, const char *s, size_t len)
{
	if (len == 0)
		return 0;
	if (s[0] == '{') {
		const char *c;
		size_t cl;
		size_t skip = brace_content(s, len, &c, &cl);
		if (skip == 0)
			return 0;
		if (cl == 1 && render_sub_char(c[0], o))
			return skip;
		morph_buf_putc(o, '_');
		morph_buf_putc(o, '(');
		render_expr(c, cl, o);
		morph_buf_putc(o, ')');
		return skip;
	}
	if (render_sub_char(s[0], o))
		return 1;
	morph_buf_putc(o, '_');
	{
		size_t b = utf8_next_codepoint_len(s, len);
		if (b == 0)
			b = 1;
		morph_buf_append(o, s, b);
		return b;
	}
}

/* ---------------- command lookup (binary search) ---------------- */

static const struct latex_cmd *find_cmd(const char *name, size_t nlen)
{
	/* bsearch needs the key to be comparable; we use a wrapper */
	for (size_t lo = 0, hi = CMD_TABLE_SIZE; lo < hi; ) {
		size_t mid = lo + (hi - lo) / 2;
		int r = strncmp(name, cmd_table[mid].name,
			 nlen > cmd_table[mid].nlen ?
			 nlen : cmd_table[mid].nlen);
		if (r == 0 && nlen == cmd_table[mid].nlen)
			return &cmd_table[mid];
		if (r < 0 || (r == 0 && nlen < cmd_table[mid].nlen))
			hi = mid;
		else
			lo = mid + 1;
	}
	return NULL;
}

/* ---------------- render a \command ---------------- */

static size_t render_cmd(const char *s, size_t len, morph_buf_t *o)
{
	if (len < 1)
		return 0;

	/* single-char escapes: \, \; \! \  */
	if (len == 1) {
		switch (s[0]) {
		case ',': morph_buf_putc(o, ' '); return 1;
		case ':': morph_buf_putc(o, ' '); return 1;
		case ';': morph_buf_putc(o, ' '); return 1;
		case '!': return 1;
		case ' ': morph_buf_putc(o, ' '); return 1;
		default:
			morph_buf_putc(o, '\\');
			morph_buf_putc(o, s[0]);
			return 1;
		}
	}

	/* extract command name */
	size_t nlen = 0;
	while (nlen < len && ((s[nlen] >= 'a' && s[nlen] <= 'z') ||
	       (s[nlen] >= 'A' && s[nlen] <= 'Z')))
		nlen++;

	if (nlen == 0) {
		/* non-alpha after backslash */
		morph_buf_putc(o, '\\');
		morph_buf_putc(o, s[0]);
		return 1;
	}

	const struct latex_cmd *cmd = find_cmd(s, nlen);
	size_t consumed = nlen;

	if (!cmd) {
		/* unrecognized command: pass through */
		morph_buf_putc(o, '\\');
		morph_buf_append(o, s, nlen);
		return consumed;
	}

	/* handle special font commands */
	if (cmd->nlen == 7 && memcmp(cmd->name, "mathbb", 6) == 0 &&
	    (cmd->name[6] == 'b')) {
		consumed += skip_brace(s + nlen, len - nlen);
		const char *c; size_t cl;
		brace_content(s + nlen, len - nlen, &c, &cl);
		render_font_group(c, cl, o, bb_map, BB_SIZE);
		return consumed;
	}
	if (cmd->nlen == 8 && memcmp(cmd->name, "mathcal", 7) == 0) {
		consumed += skip_brace(s + nlen, len - nlen);
		const char *c; size_t cl;
		brace_content(s + nlen, len - nlen, &c, &cl);
		render_font_group(c, cl, o, cal_map, CAL_SIZE);
		return consumed;
	}
	if (cmd->nlen == 9 && memcmp(cmd->name, "mathfrak", 8) == 0) {
		consumed += skip_brace(s + nlen, len - nlen);
		const char *c; size_t cl;
		brace_content(s + nlen, len - nlen, &c, &cl);
		render_font_group(c, cl, o, frk_map, FRK_SIZE);
		return consumed;
	}
	if (cmd->nlen == 7 && memcmp(cmd->name, "mathbf", 6) == 0) {
		consumed += skip_brace(s + nlen, len - nlen);
		const char *c; size_t cl;
		brace_content(s + nlen, len - nlen, &c, &cl);
		render_font_group(c, cl, o, bf_map, BF_SIZE);
		return consumed;
	}
	if (cmd->nlen == 7 && (memcmp(cmd->name, "mathrm", 6) == 0 ||
			       memcmp(cmd->name, "mathit", 6) == 0 ||
			       memcmp(cmd->name, "mathsf", 6) == 0 ||
			       memcmp(cmd->name, "mathtt", 6) == 0 ||
			       memcmp(cmd->name, "mathscr", 7) == 0)) {
		consumed += skip_brace(s + nlen, len - nlen);
		const char *c; size_t cl;
		brace_content(s + nlen, len - nlen, &c, &cl);
		/* render as-is for these fonts (no distinct unicode range
		 * that most terminals support) */
		render_expr(c, cl, o);
		return consumed;
	}

	/* sqrt: special handling */
	if (nlen == 4 && memcmp(s, "sqrt", 4) == 0) {
		size_t rest = len - nlen;
		/* check for optional [...] */
		if (rest > 0 && s[nlen] == '[') {
			size_t ei = nlen + 1;
			while (ei < len && s[ei] != ']') ei++;
			if (ei < len) {
				/* render sup-n then radical */
				for (size_t k = nlen + 1; k < ei; k++) {
					char d = s[k];
					if (d >= '0' && d <= '9')
						render_sup_char(d, o);
					else
						morph_buf_putc(o, d);
				}
				morph_buf_puts(o, "\xe2\x88\x9a"); /* √ */
				const char *c; size_t cl;
				consumed = ei + 1;
				consumed += brace_content(s + consumed,
							  len - consumed,
							  &c, &cl);
				render_expr(c, cl, o);
				return consumed;
			}
		}
		/* no optional arg */
		morph_buf_puts(o, "\xe2\x88\x9a"); /* √ */
		const char *c; size_t cl;
		size_t bg = brace_content(s + nlen, len - nlen, &c, &cl);
		consumed += bg;
		render_expr(c, cl, o);
		return consumed;
	}

	/* overline, underline, widehat, widetilde: combining char after
	 * content */
	if ((nlen == 8 && (memcmp(s, "overline", 8) == 0 ||
			   memcmp(s, "widetilde", 9) == 0)) ||
	    (nlen == 9 && memcmp(s, "widetilde", 9) == 0) ||
	    (nlen == 7 && (memcmp(s, "widehat", 7) == 0 ||
			   memcmp(s, "overline", 8) == 0)) ||
	    (nlen == 9 && memcmp(s, "underline", 9) == 0)) {
		const char *combining = NULL;
		if (memcmp(s, "overline", 8) == 0 || (nlen >= 8 && s[7] == 'e'))
			combining = "\xcc\x85";
		else if (memcmp(s, "underline", 9) == 0)
			combining = "\xcc\xb2";
		else if (memcmp(s, "widehat", 7) == 0)
			combining = "\xcc\x82";
		else if (memcmp(s, "widetilde", 9) == 0)
			combining = "\xcc\x83";
		else
			combining = cmd->uni;

		const char *c; size_t cl;
		size_t bg = brace_content(s + nlen, len - nlen, &c, &cl);
		consumed += bg;
		render_expr(c, cl, o);
		if (combining && combining[0])
			morph_buf_puts(o, combining);
		return consumed;
	}

	/* accent commands (hat, bar, vec, dot, ddot, tilde, breve,
	 * check, acute, grave) — arity 0 but actually take a following
	 * brace group or single char */
	if (cmd->arity == 0 && cmd->ulen <= 3 && cmd->uni[0] != '\0' &&
	    (unsigned char)cmd->uni[0] >= 0xc0) {
		/* likely a combining/accent character */
		size_t rest = len - nlen;
		if (rest > 0 && s[nlen] == '{') {
			const char *c; size_t cl;
			size_t bg = brace_content(s + nlen, rest, &c, &cl);
			consumed += bg;
			render_expr(c, cl, o);
			morph_buf_append(o, cmd->uni, cmd->ulen);
			return consumed;
		}
		if (rest > 0) {
			/* single char argument */
			size_t arg_len = utf8_next_codepoint_len(s + nlen, rest);
			if (arg_len == 0)
				arg_len = 1;
			render_expr(s + nlen, arg_len, o);
			morph_buf_append(o, cmd->uni, cmd->ulen);
			consumed += arg_len;
			return consumed;
		}
	}

	/* arity 2 commands: frac, binom, stackrel, overset, underset,
	 * root, dfrac, tfrac, cfrac */
	if (cmd->arity == 2) {
		const char *a, *b;
		size_t al, bl;
		size_t skip_a = brace_content(s + nlen, len - nlen, &a, &al);
		consumed += skip_a;
		size_t skip_b = brace_content(s + nlen + skip_a,
					      len - nlen - skip_a, &b, &bl);
		consumed += skip_b;

		if (nlen == 5 && memcmp(s, "binom", 5) == 0) {
			/* C(a,b) */
			morph_buf_puts(o, "C(");
			render_expr(a, al, o);
			morph_buf_putc(o, ',');
			render_expr(b, bl, o);
			morph_buf_putc(o, ')');
		} else if (nlen == 4 && memcmp(s, "root", 4) == 0) {
			/* \root{a}{b} = b√a style, rare */
			render_expr(b, bl, o);
			morph_buf_puts(o, "\xe2\x88\x9a");
			render_expr(a, al, o);
		} else {
			/* frac-style: a/b */
			render_expr(a, al, o);
			morph_buf_putc(o, '/');
			render_expr(b, bl, o);
		}
		return consumed;
	}

	/* arity 1 commands with empty uni (overbrace, underbrace,
	 * overline, etc. already handled above) */
	if (cmd->arity == 1) {
		const char *c; size_t cl;
		size_t bg = brace_content(s + nlen, len - nlen, &c, &cl);
		consumed += bg;
		if (cmd->ulen > 0) {
			render_expr(c, cl, o);
			morph_buf_append(o, cmd->uni, cmd->ulen);
		} else {
			render_expr(c, cl, o);
		}
		return consumed;
	}

	/* arity 0: simple substitution */
	if (cmd->ulen > 0)
		morph_buf_append(o, cmd->uni, cmd->ulen);

	return consumed;
}

/* ---------------- main expression renderer ---------------- */

static size_t render_expr(const char *s, size_t len, morph_buf_t *o)
{
	size_t i = 0;
	while (i < len) {
		unsigned char c = (unsigned char)s[i];

		if (c == '\\') {
			/* LaTeX command */
			size_t rest = len - i - 1;
			size_t n = render_cmd(s + i + 1, rest, o);
			i += 1 + n;
			continue;
		}

		if (c == '^') {
			/* superscript */
			i++;
			size_t n = render_sup(o, s + i, len - i);
			i += n;
			continue;
		}

		if (c == '_') {
			/* subscript */
			i++;
			size_t n = render_sub(o, s + i, len - i);
			i += n;
			continue;
		}

		if (c == '{') {
			/* skip brace, render content */
			const char *content;
			size_t clen;
			size_t skip = brace_content(s + i, len - i,
						    &content, &clen);
			if (skip > 0) {
				render_expr(content, clen, o);
				i += skip;
			} else {
				i++;
			}
			continue;
		}

		if (c == '}') {
			/* stray close brace, skip */
			i++;
			continue;
		}

		if (c == '&') {
			/* alignment tab in matrix -> space */
			morph_buf_putc(o, ' ');
			i++;
			continue;
		}

		/* multi-byte UTF-8 pass-through */
		if (c >= 0x80) {
			size_t b = utf8_next_codepoint_len(s + i, len - i);
			if (i + b <= len) {
				morph_buf_append(o, s + i, b);
				i += b;
			} else {
				i++;
			}
			continue;
		}

		/* plain ASCII */
		morph_buf_putc(o, (char)c);
		i++;
	}
	return i;
}

/* ---------------- public API ---------------- */

int latex_to_unicode(const char *latex, size_t len,
		     char *out, size_t out_cap, int flags)
{
	morph_buf_t buf;
	int rc;

	if (!latex || len == 0 || !out || out_cap == 0)
		return -EINVAL;

	(void)flags;

	rc = morph_buf_init(&buf, len + 64);
	if (rc != 0)
		return rc;

	render_expr(latex, len, &buf);

	size_t copy_len = buf.len < out_cap ? buf.len : out_cap - 1;
	memcpy(out, buf.data, copy_len);
	out[copy_len] = '\0';

	int result = (int)buf.len;
	morph_buf_cleanup(&buf);
	return result;
}
