#include <stdlib.h>
#include <string.h>
#define YYBYACC 1
#define YYMAJOR 1
#define YYMINOR 9
#define YYLEX yylex()
#define YYEMPTY -1
#define yyclearin (yychar=(YYEMPTY))
#define yyerrok (yyerrflag=0)
#define YYRECOVERING() (yyerrflag!=0)
#define YYPREFIX "yy"
#line 25 "../../usr.sbin/smtpd/parse.y"
#include "includes.h"

#include <sys/types.h>
#include <sys/time.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <net/if.h>
#include <netinet/in.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <ifaddrs.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <resolv.h>
#include <syslog.h>
#include <unistd.h>
#ifdef HAVE_UTIL_H
#include <util.h>
#endif

#include "smtpd.h"
#include "ssl.h"
#include "log.h"

TAILQ_HEAD(files, file)		 files = TAILQ_HEAD_INITIALIZER(files);
static struct file {
	TAILQ_ENTRY(file)	 entry;
	FILE			*stream;
	char			*name;
	size_t			 ungetpos;
	size_t			 ungetsize;
	u_char			*ungetbuf;
	int			 eof_reached;
	int			 lineno;
	int			 errors;
} *file, *topfile;
struct file	*pushfile(const char *, int);
int		 popfile(void);
int		 check_file_secrecy(int, const char *);
int		 yyparse(void);
int		 yylex(void);
int		 kw_cmp(const void *, const void *);
int		 lookup(char *);
int		 igetc(void);
int		 lgetc(int);
void		 lungetc(int);
int		 findeol(void);
int		 yyerror(const char *, ...)
    __attribute__((__format__ (printf, 1, 2)))
    __attribute__((__nonnull__ (1)));

TAILQ_HEAD(symhead, sym)	 symhead = TAILQ_HEAD_INITIALIZER(symhead);
struct sym {
	TAILQ_ENTRY(sym)	 entry;
	int			 used;
	int			 persist;
	char			*nam;
	char			*val;
};
int		 symset(const char *, const char *, int);
char		*symget(const char *);

struct smtpd		*conf = NULL;
static int		 errors = 0;

struct table		*table = NULL;
struct mta_limits	*limits;
static struct pki	*pki;
static struct ca	*sca;

struct dispatcher	*dsp;
struct rule		*rule;
struct filter_proc	*processor;
struct filter_config	*filter_config;
static uint32_t		 last_dynchain_id = 1;

enum listen_options {
	LO_FAMILY	= 0x000001,
	LO_PORT		= 0x000002,
	LO_SSL		= 0x000004,
	LO_FILTER      	= 0x000008,
	LO_PKI      	= 0x000010,
	LO_AUTH      	= 0x000020,
	LO_TAG      	= 0x000040,
	LO_HOSTNAME   	= 0x000080,
	LO_HOSTNAMES   	= 0x000100,
	LO_MASKSOURCE  	= 0x000200,
	LO_NODSN	= 0x000400,
	LO_SENDERS	= 0x000800,
	LO_RECEIVEDAUTH = 0x001000,
	LO_MASQUERADE	= 0x002000,
	LO_CA		= 0x004000,
	LO_PROXY       	= 0x008000,
};

#define PKI_MAX	32
static struct listen_opts {
	char	       *ifx;
	int		family;
	in_port_t	port;
	uint16_t	ssl;
	char	       *filtername;
	char	       *pki[PKI_MAX];
	int		pkicount;
	char	       *tls_ciphers;
	char	       *tls_protocols;
	char	       *ca;
	uint16_t       	auth;
	struct table   *authtable;
	char	       *tag;
	char	       *hostname;
	struct table   *hostnametable;
	struct table   *sendertable;
	uint16_t	flags;

	uint32_t       	options;
} listen_opts;

static void	create_sock_listener(struct listen_opts *);
static void	create_if_listener(struct listen_opts *);
static void	config_listener(struct listener *, struct listen_opts *);
static int	host_v4(struct listen_opts *);
static int	host_v6(struct listen_opts *);
static int	host_dns(struct listen_opts *);
static int	interface(struct listen_opts *);

int		 delaytonum(char *);
int		 is_if_in_group(const char *, const char *);

static int config_lo_mask_source(struct listen_opts *);

typedef struct {
	union {
		int64_t		 number;
		struct table	*table;
		char		*string;
		struct host	*host;
		struct mailaddr	*maddr;
	} v;
	int lineno;
} YYSTYPE;

#line 167 "../../usr.sbin/smtpd/parse.c"
#define ACTION 257
#define ADMD 258
#define ALIAS 259
#define ANY 260
#define ARROW 261
#define AUTH 262
#define AUTH_OPTIONAL 263
#define BACKUP 264
#define BOUNCE 265
#define BYPASS 266
#define CA 267
#define CERT 268
#define CHAIN 269
#define CHROOT 270
#define CIPHERS 271
#define COMMIT 272
#define COMPRESSION 273
#define CONNECT 274
#define DATA 275
#define DATA_LINE 276
#define DHE 277
#define DISCONNECT 278
#define DOMAIN 279
#define EHLO 280
#define ENABLE 281
#define ENCRYPTION 282
#define ERROR 283
#define EXPAND_ONLY 284
#define FCRDNS 285
#define FILTER 286
#define FOR 287
#define FORWARD_ONLY 288
#define FROM 289
#define GROUP 290
#define HELO 291
#define HELO_SRC 292
#define HOST 293
#define HOSTNAME 294
#define HOSTNAMES 295
#define INCLUDE 296
#define INET4 297
#define INET6 298
#define JUNK 299
#define KEY 300
#define LIMIT 301
#define LISTEN 302
#define LMTP 303
#define LOCAL 304
#define MAIL_FROM 305
#define MAILDIR 306
#define MASK_SRC 307
#define MASQUERADE 308
#define MATCH 309
#define MAX_MESSAGE_SIZE 310
#define MAX_DEFERRED 311
#define MBOX 312
#define MDA 313
#define MTA 314
#define MX 315
#define NO_DSN 316
#define NO_VERIFY 317
#define NOOP 318
#define ON 319
#define PHASE 320
#define PKI 321
#define PORT 322
#define PROC 323
#define PROC_EXEC 324
#define PROTOCOLS 325
#define PROXY_V2 326
#define QUEUE 327
#define QUIT 328
#define RCPT_TO 329
#define RDNS 330
#define RECIPIENT 331
#define RECEIVEDAUTH 332
#define REGEX 333
#define RELAY 334
#define REJECT 335
#define REPORT 336
#define REWRITE 337
#define RSET 338
#define SCHEDULER 339
#define SENDER 340
#define SENDERS 341
#define SMTP 342
#define SMTP_IN 343
#define SMTP_OUT 344
#define SMTPS 345
#define SOCKET 346
#define SRC 347
#define SRS 348
#define SUB_ADDR_DELIM 349
#define TABLE 350
#define TAG 351
#define TAGGED 352
#define TLS 353
#define TLS_REQUIRE 354
#define TTL 355
#define USER 356
#define USERBASE 357
#define VERIFY 358
#define VIRTUAL 359
#define WARN_INTERVAL 360
#define WRAPPER 361
#define STRING 362
#define NUMBER 363
#define YYERRCODE 256
const short yylhs[] =
	{                                        -1,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    7,    8,   24,   24,   24,   25,   25,   26,    3,
    3,   27,   27,   28,   29,   29,   30,   31,   31,   32,
   32,   34,    9,   10,   36,   11,   37,   35,   12,   12,
   13,   40,   13,   41,   13,   43,   14,   44,   44,   44,
   42,   42,   46,   15,   47,   47,   47,   45,   45,   16,
   16,   16,   16,   17,   18,   18,   18,   18,   19,   19,
   19,   50,   50,   50,   50,   50,   51,   51,   53,   52,
   54,   52,   55,   52,   56,   52,   57,   52,   58,   52,
   59,   52,   60,   52,   61,   52,   62,   52,   63,   63,
   63,   63,   63,   63,   63,   63,   63,   63,   63,   63,
   63,   63,   63,   63,   64,   63,   63,   66,   66,   67,
   68,   68,   69,   70,   70,   71,   21,   72,   72,   72,
   72,   72,   72,   72,   72,   72,   72,   72,   72,   72,
   72,   72,   72,   72,   72,   72,   72,   72,   72,   72,
   72,   72,   72,   72,   72,   72,   72,   72,   73,   73,
   74,   75,   75,   76,   22,   77,   77,   77,   78,   78,
   78,   78,   79,   80,   81,   82,   83,   84,   85,   86,
   87,   88,   89,   90,   91,   92,   93,   94,   94,   94,
   94,   94,   94,   95,   96,   96,   96,   97,   97,   97,
   97,   97,   97,   98,   98,   98,   98,   98,   98,   98,
   98,   99,   99,   99,   99,   99,   99,   99,   99,   99,
   99,  100,  100,  100,  100,  100,  100,  100,  100,  101,
  101,  101,  101,  101,  101,  101,  101,  103,  102,  105,
  104,  107,  106,  109,  108,  111,  110,  113,  112,  115,
  114,  117,  116,  118,  118,  118,  118,  118,  118,  118,
  118,  119,   65,   65,   23,  120,   23,  121,   23,  122,
   23,    2,    2,  123,   33,   33,  124,   49,   49,  125,
   38,   38,  126,  126,  126,   39,   39,  127,   48,   48,
  128,  129,  128,  128,  128,  128,  130,  130,  130,  130,
  130,  130,  130,  131,  130,  130,  130,  130,  130,  130,
  130,  130,  130,  130,  130,  130,  130,  130,  130,  130,
  130,  130,  130,  130,  130,  130,  130,  132,  132,  133,
  134,  135,  135,  136,  136,  137,   20,    1,  138,    1,
    5,  139,    5,    6,    4,    4,
};
const short yylen[] =
	{                                         2,
    0,    2,    3,    3,    3,    3,    3,    3,    3,    3,
    3,    3,    3,    3,    3,    3,    3,    3,    3,    3,
    3,    2,    3,    2,    1,    0,    2,    0,    2,    1,
    0,    1,    1,    3,    2,    3,    1,    2,    3,    1,
    1,    0,    4,    2,    0,    4,    2,    1,    3,    4,
    3,    0,    7,    0,    4,    0,    4,    2,    2,    2,
    2,    0,    0,    5,    2,    2,    2,    2,    0,    2,
    2,    3,    3,    3,    3,    3,    3,    3,    3,    4,
    3,    2,    2,    2,    2,    2,    2,    0,    0,    3,
    0,    3,    0,    4,    0,    4,    0,    5,    0,    4,
    0,    5,    0,    4,    0,    3,    0,    3,    2,    2,
    2,    2,    2,    2,    2,    2,    3,    1,    2,    2,
    1,    2,    2,    2,    0,    6,    1,    2,    0,    2,
    1,    1,    2,    2,    0,    0,    5,    3,    4,    3,
    4,    2,    2,    3,    4,    3,    4,    3,    4,    3,
    3,    3,    4,    5,    3,    4,    5,    3,    4,    5,
    4,    5,    3,    3,    4,    5,    4,    5,    2,    0,
    1,    1,    2,    0,    4,    1,    1,    1,    2,    2,
    2,    2,    2,    2,    3,    4,    3,    4,    3,    4,
    2,    3,    4,    3,    4,    3,    4,    1,    1,    1,
    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,
    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,
    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,
    1,    1,    1,    1,    1,    1,    1,    1,    1,    1,
    1,    1,    1,    1,    1,    1,    1,    0,    5,    0,
    5,    0,    5,    0,    5,    0,    5,    0,    5,    0,
    5,    0,    5,    1,    1,    1,    1,    1,    1,    1,
    1,    1,    2,    3,    4,    0,    6,    0,    5,    0,
    8,    1,    1,    1,    3,    1,    2,    2,    0,    2,
    2,    0,    1,    1,    2,    2,    0,    2,    2,    0,
    2,    0,    6,    1,    1,    2,    1,    1,    2,    2,
    2,    2,    2,    0,    6,    1,    2,    1,    1,    2,
    2,    2,    2,    2,    1,    1,    2,    2,    2,    2,
    2,    1,    1,    1,    1,    2,    3,    1,    1,    2,
    2,    2,    0,    2,    0,    0,    4,    3,    0,    7,
    1,    0,    5,    3,    1,    1,
};
const short yydefred[] =
	{                                      1,
    0,    0,    0,    0,    0,    0,    0,    0,  346,  174,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    2,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,   21,
  136,   44,   42,   45,    0,   22,    0,    0,    0,    0,
    0,    0,   56,    0,   70,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,   17,    3,    4,    5,
    6,    7,    8,    9,   10,   11,   12,   13,   14,   15,
   16,   18,   19,   20,    0,    0,    0,  280,  278,    0,
    0,    0,   30,    0,    0,    0,    0,   49,    0,    0,
    0,    0,   51,    0,   63,   72,   73,    0,   74,    0,
   76,    0,   75,    0,  283,  282,   77,   78,    0,   79,
   81,  348,    0,   23,  107,  105,    0,    0,   89,    0,
    0,  131,  132,    0,  284,    0,  286,    0,   46,   48,
    0,    0,  275,  276,    0,    0,  347,  338,  339,    0,
    0,    0,    0,    0,    0,    0,  142,  169,    0,  172,
  175,  287,  291,   50,    0,  293,  294,    0,   55,    0,
    0,    0,    0,   57,    0,    0,  298,  299,  290,  288,
   80,    0,    0,    0,    0,   93,    0,    0,    0,  103,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,  127,    0,    0,  130,    0,    0,  137,
    0,   47,    0,  254,  262,  248,  260,  252,  250,  256,
  258,  264,  265,  266,  267,  268,  269,  270,  271,  279,
    0,    0,  304,  305,    0,    0,  340,    0,    0,    0,
    0,    0,    0,    0,  307,  308,  332,  334,    0,    0,
    0,  335,  333,    0,    0,    0,  318,    0,    0,  341,
    0,  351,    0,    0,  144,  355,  356,  164,    0,  163,
    0,  152,    0,  151,    0,    0,  150,    0,    0,  140,
    0,  146,    0,  148,    0,  138,  171,  173,   52,  295,
  296,   58,   60,   59,   61,    0,    0,    0,   64,    0,
    0,    0,    0,    0,    0,    0,    0,    0,  108,  106,
  101,    0,    0,   97,    0,   92,   90,    0,  123,    0,
  112,  113,  120,  124,    0,  109,  110,  119,  116,  111,
  114,  115,  122,  128,  133,  134,  285,    0,    0,    0,
    0,    0,    0,    0,    0,    0,  277,  301,    0,  306,
  342,  327,  328,  324,  321,  313,    0,  330,  331,  323,
  310,  311,  309,  312,  322,    0,  317,  329,  320,  344,
  145,  352,    0,    0,  165,    0,  167,    0,  159,    0,
  161,    0,  156,    0,  153,  141,  147,  149,  139,    0,
   67,   66,   65,   68,   27,    0,    0,   41,    0,   40,
    0,   83,   82,   85,   84,   86,   87,    0,  100,   94,
    0,   96,  104,  117,    0,  272,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,  337,    0,
  354,  166,  168,  160,  162,  157,  154,   53,   33,   32,
    0,    0,    0,    0,   35,   25,    0,   38,  350,  102,
   98,    0,  281,    0,  273,    0,  198,  199,  201,  200,
  203,  202,  208,  209,  210,  211,  212,  213,    0,    0,
  240,  241,  242,  243,  244,  245,  246,  247,    0,    0,
  204,    0,  232,  233,  234,  235,  236,  237,  238,  239,
    0,    0,  205,  206,  207,    0,    0,  214,  215,  216,
  217,  218,  219,  220,  221,    0,    0,  222,  223,  224,
  225,  226,  227,  228,  229,  230,  231,    0,    0,    0,
    0,   34,    0,   24,    0,   36,   37,   39,    0,  274,
    0,  183,    0,    0,    0,  178,    0,  177,    0,    0,
    0,  255,  176,    0,  263,  249,  261,  253,  251,  257,
    0,  259,    0,    0,  353,  126,    0,  192,    0,  189,
    0,  185,    0,  187,  180,  179,  182,  181,    0,  194,
    0,  196,  303,  315,  193,  190,  186,  188,  195,  197,
};
const short yydgoto[] =
	{                                       1,
   22,  117,  470,  265,  266,  267,   23,   24,   25,   26,
   27,   28,   29,   30,   31,   32,   33,   34,   35,   36,
   37,   38,   39,  444,  302,  446,  441,  397,  398,  399,
  400,  401,  136,   86,  139,   87,  140,   98,  169,  390,
  102,  174,  104,  175,  299,  176,  300,  109,  113,  308,
  309,  132,  189,  188,  313,  315,  411,  312,  408,  318,
  184,  183,  206,  325,  417,  207,  133,  134,  209,  210,
   85,   95,   96,  288,  161,   48,  542,  543,  457,  458,
  459,  460,  461,  462,  493,  494,  465,  466,  467,  476,
  477,  515,  516,  495,  482,  496,  469,  506,  518,  491,
  479,  222,  341,  223,  344,  224,  343,  225,  339,  226,
  345,  227,  346,  228,  342,  229,  340,  230,  418,  231,
  142,  141,  137,   99,  114,  170,  110,  236,  349,  259,
  357,  147,  148,  149,  237,  260,   47,  123,  430,
};
const short yysindex[] =
	{                                      0,
   -6,   37, -311, -298, -293, -291, -286, -282,    0,    0,
 -264, -243, -280, -262, -220, -192, -232, -252, -255,   70,
    0,  126,  132,  146,  150,  156,  158,  160,  161,  167,
  168,  170,  180,  182,  185,  187,  194,  197,  204,    0,
    0,    0,    0,    0,   26,    0, -144,  193, -125, -119,
  -69, -131,    0, -118,    0, -117, -115, -113, -107, -104,
 -307, -102, -233,  -97,  -90,  -88,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,  350,  -86,   10,    0,    0,  -83,
  -81, -305,    0, -148,  193, -237,  -80,    0, -125,  -77,
    8, -275,    0, -207,    0,    0,    0,  -75,    0, -113,
    0,  -66,    0, -104,    0,    0,    0,    0,  -68,    0,
    0,    0,  176,    0,    0,    0,  -58, -273,    0,  -52,
  505,    0,    0,  -54,    0,  274,    0,  -39,    0,    0,
  201,  -89,    0,    0, -211,  495,    0,    0,    0,  -42,
  -91, -111,  -35,  -33,  -31,  -28,    0,    0,  -30,    0,
    0,    0,    0,    0,  -22,    0,    0,  -37,    0, -275,
  -21,  -19,  -14,    0, -207, -246,    0,    0,    0,    0,
    0,  329, -194, -194,   33,    0,   65, -194, -194,    0,
  -51,   54,    9,   31,  -51,   32,   34,  -51,  -51,   39,
   41,   52,  -51,    0,   82,  505,    0,   61,  -54,    0,
  -86,    0,  329,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
 -246,   66,    0,    0,   71, -211,    0,  -51,  -51,   75,
   77,   80,   85,  -51,    0,    0,    0,    0,   88, -279,
   93,    0,    0,  -51,   74,   95,    0,  103,  495,    0,
  -51,    0,  329,  100,    0,    0,    0,    0,  -24,    0,
  -17,    0,  -11,    0,   -8,   14,    0,   51,  -51,    0,
  -51,    0,  -51,    0,  -51,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,  106,  114,  118,    0, -246,
  329,  125,  -51,  127,  -51,  -51,  128, -194,    0,    0,
    0, -194, -194,    0, -194,    0,    0, -194,    0,  135,
    0,    0,    0,    0,  289,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,  139,  137,  179,
  215,  216,  219,  224,  234,  239,    0,    0,  321,    0,
    0,    0,    0,    0,    0,    0,  390,    0,    0,    0,
    0,    0,    0,    0,    0,  244,    0,    0,    0,    0,
    0,    0,  494,  -51,    0,  -51,    0,  -51,    0,  -51,
    0,  -51,    0,  -51,    0,    0,    0,    0,    0, -275,
    0,    0,    0,    0,    0,  -46,   50,    0,   50,    0,
  432,    0,    0,    0,    0,    0,    0, -194,    0,    0,
 -194,    0,    0,    0,  329,    0,  433,   50,  193,  193,
  193,  193,  193,  193,  193,  193,  329,  329,    0,  125,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
  198,  329,  329,  200,    0,    0,  208,    0,    0,    0,
    0,  139,    0,  139,    0, -158,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,  287,  -63,
    0,    0,    0,    0,    0,    0,    0,    0,  147, -151,
    0,  287,    0,    0,    0,    0,    0,    0,    0,    0,
  287,  -61,    0,    0,    0,  287,  287,    0,    0,    0,
    0,    0,    0,    0,    0,  287,  173,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,  287,  139,  139,
  444,    0,    0,    0,  -46,    0,    0,    0,  447,    0,
   53,    0,   59,   64,   78,    0,  214,    0,  225,  229,
  230,    0,    0,   86,    0,    0,    0,    0,    0,    0,
   87,    0,  456,  469,    0,    0,  -51,    0,  -51,    0,
  -51,    0,  -51,    0,    0,    0,    0,    0,  -51,    0,
  -51,    0,    0,    0,    0,    0,    0,    0,    0,    0,};
const short yyrindex[] =
	{                                      0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,  450,  585,    0,
   -5,    0,    0,    0,    0,  156,    0,  590,    0,  594,
    0,    0,    0,    0,  486,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,  450,    0,    0,    0,  585,    0,
    0,  600,    0,  603,    0,    0,    0,    0,    0,  590,
    0,    0,    0,  594,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    4,    0,    0,
   -3,    0,    0,  604,    0,  605,    0,    0,    0,    0,
    0,    0,    0,    0,  607,  609,    0,    0,    0,  -23,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,  600,
    0,    0,    0,    0,  603,  146,    0,    0,    0,    0,
    0,  259,   -2,   -2,   11,    0,   18,   -2,   -2,    0,
    0,  138,    0,    0,    0,  504,    0,    0,    0,    0,
    0,    0,    0,    0,  174,   -3,    0,    0,  604,    0,
    0,    0,  259,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
  146,  510,    0,    0,    0,  607,    0,  210,  252,    0,
    0,  512,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,  304,    0,    0,  345,  609,    0,
    0,    0,  259,    0,    0,    0,    0,    0,    0,    0,
    0,    0,  -16,    0,    0,  165,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,  146,
 -114,    0,    0,    0,    0,    0,    0,   -2,    0,    0,
    0,   -2,   -2,    0,   -2,    0,    0,   -2,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,  397,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,  600,
    0,    0,    0,    0,    0,   -4, -109,    0, -109,    0,
    0,    0,    0,    0,    0,    0,    0,   -2,    0,    0,
   -2,    0,    0,    0,  259,    0,    0, -109,  249,  207,
 -130,  207,  104,  104,  207,  435,  259,  259,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0, -114,  259,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0, -106,    0,    0,    0,    0,    0,    0,    0,
  366,    0,    0,  508,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,};
const short yygindex[] =
	{                                      0,
    0,    0,  -36, -153,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0, -345,  317,    0,    0,    0,  184,    0,
  190,  211,    0,    0,    0,    0,    0,  547, -157,    0,
    0,  472,    0,    0, -198,    0,    0,  538,  536,    0,
 -154,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0, -375,  445,    0,    0,    0,  460,
    0,    0,  559,    0,    0,    0,  332,  202,    0,    0,
    0,    0,    0,    0,  163,  305, -214,  119,  327,   49,
   84,    0,    0,  253,    0,  256,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,  465,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,  446,  426,    0,    0,    0,
};
#define YYTABLESIZE 858
const short yytable[] =
	{                                     280,
  282,  284,  286,   21,   54,   37,  129,   88,  264,  143,
   28,   94,  291,   91,  440,   28,  158,  264,   27,  159,
   99,  166,  167,  296,  264,  186,  264,   95,  264,  310,
  119,  264,  347,  316,  317,  264,   49,  319,   59,   37,
  145,  323,  264,  297,  327,  328,   40,   63,  264,  332,
   41,  264,   55,  447,  115,  116,  146,   51,   94,  442,
  171,   56,  361,   42,  303,  362,   43,   52,   60,  172,
   44,  263,  454,  264,  232,   45,  529,   61,  530,   46,
  263,   53,  363,  364,  352,  353,  168,  263,  187,  263,
  359,  263,  173,  443,  263,  233,   50,  160,  263,   54,
  366,  394,   64,  531,  234,  263,   65,  371,   58,  298,
  264,  263,  264,  150,  263,  375,   62,  377,  264,  379,
   37,  381,  383,  264,  385,  386,  532,  387,  120,  388,
   66,  389,  533,  532,   57,   67,  263,  264,  151,  235,
  152,   68,  153,  553,  554,  264,  264,  118,  272,  402,
  273,  404,  405,  407,   31,   69,  154,  409,  410,   70,
  412,  304,  305,  413,  306,   71,  307,   72,  268,   73,
   74,  534,  214,  263,   92,  263,   75,   76,  534,   77,
  155,  263,  215,  121,  216,  217,  263,  269,  535,   78,
  218,   79,  274,  275,   80,  535,   81,  155,  531,   31,
  263,  219,  156,   82,  157,  473,   83,  485,  263,  263,
  500,  510,  270,   84,  439,  220,   31,  101,  276,  325,
  432,  532,  433,  532,  434,   93,  435,  533,  436,  533,
  437,  103,  438,  143,  277,  278,   97,  271,  143,  221,
  158,  544,  100,  105,  106,  158,  107,   28,  108,    2,
    3,    4,   26,  450,  111,   29,  451,  112,    5,  118,
    6,  326,   91,  143,  121,  143,  534,  143,  534,   99,
  158,  122,  158,  124,  158,  135,   95,  138,  143,    7,
  144,  143,  162,  535,  164,  535,  165,  177,  158,    8,
  261,   54,   54,  181,   88,    9,  179,  279,  182,  281,
  208,  283,   10,  185,  285,  143,   11,   12,  374,  190,
  262,  143,  158,  316,   13,  376,   14,  211,  158,  262,
   15,  378,  212,  213,  380,  290,  262,  143,  262,  143,
  262,  287,   16,  262,  158,   17,  158,  262,  301,  289,
  292,   18,  293,   19,  262,   89,  382,  294,   90,   91,
  262,  129,   88,  262,  319,   20,   54,   37,   91,   91,
   91,  311,   91,  314,   91,   99,   99,   99,  320,   99,
  321,   99,   95,   95,   95,  262,   95,  558,   95,  560,
  562,  564,  456,  384,  480,  557,  492,  492,   31,  507,
  570,  559,  322,  324,   31,  326,  561,  572,  333,  118,
  329,  118,  330,  575,  118,  576,  336,  577,  118,  578,
  563,  415,  262,  331,  262,  579,  118,  580,  569,  571,
  262,  155,  335,  118,  537,  262,  155,  348,  118,  118,
  118,  367,  350,   31,  531,  121,  354,  121,  355,  262,
  121,  356,  118,  427,  121,  419,  358,  262,  262,  360,
   31,  155,  121,  155,  365,  155,  368,  532,  118,  121,
  369,  373,  118,  533,  121,  121,  121,  391,   31,  155,
  488,  325,  325,  503,  513,  392,  325,  544,  121,  393,
  325,  539,  540,  541,  118,  118,  396,  420,  403,  406,
  118,   31,  118,  155,  121,  325,  414,   31,  121,  155,
  416,  551,  534,  325,  325,  489,  325,  325,  504,  514,
   31,   31,  428,  326,  326,  155,  325,  155,  326,  535,
  121,  121,  326,  421,  422,  325,  121,  423,  121,  338,
  325,  325,  424,   31,  325,  325,   31,  326,  474,   31,
  486,  325,  425,  501,  511,  326,  326,  426,  326,  326,
  325,  429,  536,   31,  325,  431,  449,  453,  326,  522,
  325,  525,  325,  325,  537,  316,  316,  326,  555,  527,
  316,  556,  326,  326,  316,  565,  326,  326,   31,  372,
  573,  463,  471,  326,  483,  538,  566,  498,  508,  316,
  567,  568,  326,  574,  292,   31,  326,  316,  316,  300,
  316,  316,  326,  289,  326,  326,  319,  319,  349,  297,
  316,  319,   62,  135,   43,  319,  343,  395,  345,  316,
   28,  539,  540,  541,  316,  316,  125,  526,  316,  316,
  319,  191,  302,  125,  314,  316,  528,  126,  319,  319,
  521,  319,  319,  191,  316,  163,  295,  178,  316,  180,
  334,  319,  127,  158,  316,  128,  316,  316,  336,  336,
  319,  129,  130,  336,  191,  319,  319,  336,  336,  319,
  319,  468,  478,  481,  490,  337,  319,  505,  517,  497,
  545,  351,  336,  131,  370,  319,    0,    0,    0,  319,
  336,  336,    0,  336,  336,  319,   31,  319,  319,    0,
  191,  191,  191,  336,    0,    0,  170,    0,    0,    0,
    0,   31,  336,  445,    0,  448,    0,  336,  336,   31,
    0,  336,  336,  464,  472,   31,  484,    0,  336,  499,
  509,  452,    0,    0,  455,    0,   31,  336,   31,   31,
   31,  336,    0,  519,  520,    0,  475,  336,  487,  336,
  336,  502,  512,    0,   31,    0,  238,  239,  523,  524,
    0,  240,    0,   31,   31,  241,  191,    0,  192,    0,
    0,  193,    0,  184,    0,  194,    0,    0,   31,    0,
  242,   31,    0,  195,  170,  184,    0,    0,  243,  244,
  196,  245,  246,    0,    0,  197,  198,  199,    0,    0,
   31,  247,   31,    0,    0,    0,  184,    0,    0,  200,
  248,    0,    0,  546,    0,  249,  250,    0,    0,  251,
  252,    0,  547,    0,    0,  201,  253,  548,  549,  202,
    0,    0,    0,    0,    0,  254,    0,  550,    0,  255,
    0,    0,  184,  184,  184,  256,    0,  257,  258,  552,
    0,  203,  204,    0,    0,    0,    0,  205,
};
const short yycheck[] =
	{                                     153,
  154,  155,  156,   10,   10,   10,   10,   10,   60,   33,
  125,   48,  170,   10,   61,  125,   33,   60,  125,  257,
   10,  297,  298,  270,   60,  299,   60,   10,   60,  184,
  264,   60,  231,  188,  189,   60,  301,  191,  271,   44,
  346,  195,   60,  290,  198,  199,   10,  300,   60,  203,
  362,   60,  273,  399,  362,  363,  362,  301,   95,   10,
  268,  282,  342,  362,  259,  345,  360,  311,  301,  277,
  362,  123,  418,   60,  286,  362,  452,  310,  454,  362,
  123,  362,  362,  363,  238,  239,  362,  123,  362,  123,
  244,  123,  300,   44,  123,  307,  361,  335,  123,  362,
  254,  300,  355,  262,  316,  123,  362,  261,  301,  356,
   60,  123,   60,  262,  123,  269,  349,  271,   60,  273,
  125,  275,  276,   60,  278,  279,  285,  281,  362,  283,
   61,  285,  291,  285,  355,   10,  123,   60,  287,  351,
  289,   10,  291,  519,  520,   60,   60,   10,  260,  303,
  262,  305,  306,  308,  285,   10,  305,  312,  313,   10,
  315,  356,  357,  318,  359,   10,  361,   10,  260,   10,
   10,  330,  262,  123,  319,  123,   10,   10,  330,   10,
  329,  123,  272,   10,  274,  275,  123,  279,  347,   10,
  280,   10,  304,  305,   10,  347,   10,   33,  262,  330,
  123,  291,  351,   10,  353,  420,   10,  422,  123,  123,
  425,  426,  304,   10,  261,  305,  347,  287,  330,   10,
  374,  285,  376,  285,  378,   33,  380,  291,  382,  291,
  384,  363,  390,  257,  346,  347,  362,  329,  262,  329,
  257,  305,  362,  362,  362,  262,  362,  362,  362,  256,
  257,  258,  362,  408,  362,  362,  411,  362,  265,  362,
  267,   10,  259,  287,  362,  289,  330,  291,  330,  259,
  287,  362,  289,  362,  291,  362,  259,  268,  362,  286,
  362,  305,  363,  347,  362,  347,  279,  363,  305,  296,
  333,  297,  298,  362,  269,  302,  363,  333,  123,  333,
  355,  333,  309,  362,  333,  329,  313,  314,  333,  362,
  362,  335,  329,   10,  321,  333,  323,   44,  335,  362,
  327,  333,  362,  123,  333,  363,  362,  351,  362,  353,
  362,  362,  339,  362,  351,  342,  353,  362,   10,  362,
  362,  348,  362,  350,  362,  320,  333,  362,  323,  324,
  362,  355,  355,  362,   10,  362,  362,  362,  355,  356,
  357,  329,  359,  299,  361,  355,  356,  357,  315,  359,
  362,  361,  355,  356,  357,  362,  359,  531,  361,  533,
  534,  535,  419,  333,  421,  333,  423,  424,  285,  426,
  544,  333,  362,  362,  291,  362,  333,  551,  317,  262,
  362,  264,  362,  557,  267,  559,   10,  561,  271,  563,
  333,  123,  362,  362,  362,  569,  279,  571,  333,  333,
  362,  257,  362,  286,  278,  362,  262,  362,  291,  292,
  293,  358,  362,  330,  262,  262,  362,  264,  362,  362,
  267,  362,  305,  123,  271,  309,  362,  362,  362,  362,
  347,  287,  279,  289,  362,  291,  362,  285,  321,  286,
  358,  362,  325,  291,  291,  292,  293,  362,  262,  305,
  422,  262,  263,  425,  426,  362,  267,  305,  305,  362,
  271,  335,  336,  337,  347,  348,  362,  309,  362,  362,
  353,  285,  355,  329,  321,  286,  362,  291,  325,  335,
  362,  329,  330,  294,  295,  422,  297,  298,  425,  426,
  262,  305,  123,  262,  263,  351,  307,  353,  267,  347,
  347,  348,  271,  309,  309,  316,  353,  309,  355,  213,
  321,  322,  309,  285,  325,  326,  330,  286,  420,  291,
  422,  332,  309,  425,  426,  294,  295,  309,  297,  298,
  341,  308,  266,  347,  345,   62,  125,  125,  307,  362,
  351,  362,  353,  354,  278,  262,  263,  316,  125,  362,
  267,  125,  321,  322,  271,  362,  325,  326,  330,  263,
  125,  419,  420,  332,  422,  299,  362,  425,  426,  286,
  362,  362,  341,  125,   10,  347,  345,  294,  295,   10,
  297,  298,  351,   10,  353,  354,  262,  263,  123,   10,
  307,  267,   10,   10,   10,  271,   10,  301,   10,  316,
  362,  335,  336,  337,  321,  322,  123,  444,  325,  326,
  286,  266,  123,  284,  123,  332,  447,  288,  294,  295,
  430,  297,  298,  278,  341,   99,  175,  110,  345,  114,
  206,  307,  303,   95,  351,  306,  353,  354,  262,  263,
  316,  312,  313,  267,  299,  321,  322,  271,  209,  325,
  326,  419,  420,  421,  422,  211,  332,  425,  426,  424,
  479,  236,  286,  334,  259,  341,   -1,   -1,   -1,  345,
  294,  295,   -1,  297,  298,  351,  262,  353,  354,   -1,
  335,  336,  337,  307,   -1,   -1,  257,   -1,   -1,   -1,
   -1,  262,  316,  397,   -1,  399,   -1,  321,  322,  285,
   -1,  325,  326,  419,  420,  291,  422,   -1,  332,  425,
  426,  415,   -1,   -1,  418,   -1,  287,  341,  289,  305,
  291,  345,   -1,  427,  428,   -1,  420,  351,  422,  353,
  354,  425,  426,   -1,  305,   -1,  262,  263,  442,  443,
   -1,  267,   -1,  329,  330,  271,  262,   -1,  264,   -1,
   -1,  267,   -1,  266,   -1,  271,   -1,   -1,  329,   -1,
  286,  347,   -1,  279,  335,  278,   -1,   -1,  294,  295,
  286,  297,  298,   -1,   -1,  291,  292,  293,   -1,   -1,
  351,  307,  353,   -1,   -1,   -1,  299,   -1,   -1,  305,
  316,   -1,   -1,  482,   -1,  321,  322,   -1,   -1,  325,
  326,   -1,  491,   -1,   -1,  321,  332,  496,  497,  325,
   -1,   -1,   -1,   -1,   -1,  341,   -1,  506,   -1,  345,
   -1,   -1,  335,  336,  337,  351,   -1,  353,  354,  518,
   -1,  347,  348,   -1,   -1,   -1,   -1,  353,
};
#define YYFINAL 1
#ifndef YYDEBUG
#define YYDEBUG 0
#endif
#define YYMAXTOKEN 363
#if YYDEBUG
const char * const yyname[] =
	{
"end-of-file",0,0,0,0,0,0,0,0,0,"'\\n'",0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,"'!'",0,0,0,0,0,0,0,0,0,0,"','",0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,"'<'","'='",
"'>'",0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,"'{'",0,"'}'",0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,"ACTION","ADMD","ALIAS","ANY","ARROW","AUTH","AUTH_OPTIONAL","BACKUP",
"BOUNCE","BYPASS","CA","CERT","CHAIN","CHROOT","CIPHERS","COMMIT","COMPRESSION",
"CONNECT","DATA","DATA_LINE","DHE","DISCONNECT","DOMAIN","EHLO","ENABLE",
"ENCRYPTION","ERROR","EXPAND_ONLY","FCRDNS","FILTER","FOR","FORWARD_ONLY",
"FROM","GROUP","HELO","HELO_SRC","HOST","HOSTNAME","HOSTNAMES","INCLUDE",
"INET4","INET6","JUNK","KEY","LIMIT","LISTEN","LMTP","LOCAL","MAIL_FROM",
"MAILDIR","MASK_SRC","MASQUERADE","MATCH","MAX_MESSAGE_SIZE","MAX_DEFERRED",
"MBOX","MDA","MTA","MX","NO_DSN","NO_VERIFY","NOOP","ON","PHASE","PKI","PORT",
"PROC","PROC_EXEC","PROTOCOLS","PROXY_V2","QUEUE","QUIT","RCPT_TO","RDNS",
"RECIPIENT","RECEIVEDAUTH","REGEX","RELAY","REJECT","REPORT","REWRITE","RSET",
"SCHEDULER","SENDER","SENDERS","SMTP","SMTP_IN","SMTP_OUT","SMTPS","SOCKET",
"SRC","SRS","SUB_ADDR_DELIM","TABLE","TAG","TAGGED","TLS","TLS_REQUIRE","TTL",
"USER","USERBASE","VERIFY","VIRTUAL","WARN_INTERVAL","WRAPPER","STRING",
"NUMBER",
};
const char * const yyrule[] =
	{"$accept : grammar",
"grammar :",
"grammar : grammar '\\n'",
"grammar : grammar include '\\n'",
"grammar : grammar varset '\\n'",
"grammar : grammar bounce '\\n'",
"grammar : grammar admd '\\n'",
"grammar : grammar ca '\\n'",
"grammar : grammar mda '\\n'",
"grammar : grammar mta '\\n'",
"grammar : grammar pki '\\n'",
"grammar : grammar proc '\\n'",
"grammar : grammar queue '\\n'",
"grammar : grammar scheduler '\\n'",
"grammar : grammar smtp '\\n'",
"grammar : grammar srs '\\n'",
"grammar : grammar listen '\\n'",
"grammar : grammar table '\\n'",
"grammar : grammar dispatcher '\\n'",
"grammar : grammar match '\\n'",
"grammar : grammar filter '\\n'",
"grammar : grammar error '\\n'",
"include : INCLUDE STRING",
"varset : STRING '=' STRING",
"comma : ',' optnl",
"comma : nl",
"comma :",
"optnl : '\\n' optnl",
"optnl :",
"nl : '\\n' optnl",
"negation : '!'",
"negation :",
"assign : '='",
"assign : ARROW",
"keyval : STRING assign STRING",
"keyval_list : keyval optnl",
"keyval_list : keyval comma keyval_list",
"stringel : STRING",
"string_list : stringel optnl",
"string_list : stringel comma string_list",
"tableval_list : string_list",
"tableval_list : keyval_list",
"$$1 :",
"bounce : BOUNCE WARN_INTERVAL $$1 bouncedelays",
"admd : ADMD STRING",
"$$2 :",
"ca : CA STRING $$2 ca_params",
"ca_params_opt : CERT STRING",
"ca_params : ca_params_opt",
"mda : MDA LIMIT limits_mda",
"mda : MDA WRAPPER STRING STRING",
"mta : MTA MAX_DEFERRED NUMBER",
"$$3 :",
"mta : MTA LIMIT FOR DOMAIN STRING $$3 limits_mta",
"$$4 :",
"mta : MTA LIMIT $$4 limits_mta",
"$$5 :",
"pki : PKI STRING $$5 pki_params",
"pki_params_opt : CERT STRING",
"pki_params_opt : KEY STRING",
"pki_params_opt : DHE STRING",
"pki_params : pki_params_opt pki_params",
"pki_params :",
"$$6 :",
"proc : PROC STRING STRING $$6 proc_params",
"proc_params_opt : USER STRING",
"proc_params_opt : GROUP STRING",
"proc_params_opt : CHROOT STRING",
"proc_params : proc_params_opt proc_params",
"proc_params :",
"queue : QUEUE COMPRESSION",
"queue : QUEUE ENCRYPTION",
"queue : QUEUE ENCRYPTION STRING",
"queue : QUEUE TTL STRING",
"scheduler : SCHEDULER LIMIT limits_scheduler",
"smtp : SMTP LIMIT limits_smtp",
"smtp : SMTP CIPHERS STRING",
"smtp : SMTP MAX_MESSAGE_SIZE size",
"smtp : SMTP SUB_ADDR_DELIM STRING",
"srs : SRS KEY STRING",
"srs : SRS KEY BACKUP STRING",
"srs : SRS TTL STRING",
"dispatcher_local_option : USER STRING",
"dispatcher_local_option : ALIAS tables",
"dispatcher_local_option : VIRTUAL tables",
"dispatcher_local_option : USERBASE tables",
"dispatcher_local_option : WRAPPER STRING",
"dispatcher_local_options : dispatcher_local_option dispatcher_local_options",
"dispatcher_local_options :",
"$$7 :",
"dispatcher_local : MBOX $$7 dispatcher_local_options",
"$$8 :",
"dispatcher_local : MAILDIR $$8 dispatcher_local_options",
"$$9 :",
"dispatcher_local : MAILDIR JUNK $$9 dispatcher_local_options",
"$$10 :",
"dispatcher_local : MAILDIR STRING $$10 dispatcher_local_options",
"$$11 :",
"dispatcher_local : MAILDIR STRING JUNK $$11 dispatcher_local_options",
"$$12 :",
"dispatcher_local : LMTP STRING $$12 dispatcher_local_options",
"$$13 :",
"dispatcher_local : LMTP STRING RCPT_TO $$13 dispatcher_local_options",
"$$14 :",
"dispatcher_local : MDA STRING $$14 dispatcher_local_options",
"$$15 :",
"dispatcher_local : FORWARD_ONLY $$15 dispatcher_local_options",
"$$16 :",
"dispatcher_local : EXPAND_ONLY $$16 dispatcher_local_options",
"dispatcher_remote_option : HELO STRING",
"dispatcher_remote_option : HELO_SRC tables",
"dispatcher_remote_option : PKI STRING",
"dispatcher_remote_option : CA STRING",
"dispatcher_remote_option : CIPHERS STRING",
"dispatcher_remote_option : PROTOCOLS STRING",
"dispatcher_remote_option : SRC tables",
"dispatcher_remote_option : MAIL_FROM STRING",
"dispatcher_remote_option : BACKUP MX STRING",
"dispatcher_remote_option : BACKUP",
"dispatcher_remote_option : HOST tables",
"dispatcher_remote_option : DOMAIN tables",
"dispatcher_remote_option : TLS",
"dispatcher_remote_option : TLS NO_VERIFY",
"dispatcher_remote_option : AUTH tables",
"dispatcher_remote_option : FILTER STRING",
"$$17 :",
"dispatcher_remote_option : FILTER $$17 '{' optnl filter_list '}'",
"dispatcher_remote_option : SRS",
"dispatcher_remote_options : dispatcher_remote_option dispatcher_remote_options",
"dispatcher_remote_options :",
"dispatcher_remote : RELAY dispatcher_remote_options",
"dispatcher_type : dispatcher_local",
"dispatcher_type : dispatcher_remote",
"dispatcher_option : TTL STRING",
"dispatcher_options : dispatcher_option dispatcher_options",
"dispatcher_options :",
"$$18 :",
"dispatcher : ACTION STRING $$18 dispatcher_type dispatcher_options",
"match_option : negation TAG tables",
"match_option : negation TAG REGEX tables",
"match_option : negation HELO tables",
"match_option : negation HELO REGEX tables",
"match_option : negation TLS",
"match_option : negation AUTH",
"match_option : negation AUTH tables",
"match_option : negation AUTH REGEX tables",
"match_option : negation MAIL_FROM tables",
"match_option : negation MAIL_FROM REGEX tables",
"match_option : negation RCPT_TO tables",
"match_option : negation RCPT_TO REGEX tables",
"match_option : negation FROM SOCKET",
"match_option : negation FROM LOCAL",
"match_option : negation FROM ANY",
"match_option : negation FROM SRC tables",
"match_option : negation FROM SRC REGEX tables",
"match_option : negation FROM RDNS",
"match_option : negation FROM RDNS tables",
"match_option : negation FROM RDNS REGEX tables",
"match_option : negation FROM AUTH",
"match_option : negation FROM AUTH tables",
"match_option : negation FROM AUTH REGEX tables",
"match_option : negation FROM MAIL_FROM tables",
"match_option : negation FROM MAIL_FROM REGEX tables",
"match_option : negation FOR LOCAL",
"match_option : negation FOR ANY",
"match_option : negation FOR DOMAIN tables",
"match_option : negation FOR DOMAIN REGEX tables",
"match_option : negation FOR RCPT_TO tables",
"match_option : negation FOR RCPT_TO REGEX tables",
"match_options : match_option match_options",
"match_options :",
"match_dispatcher : STRING",
"action : REJECT",
"action : ACTION match_dispatcher",
"$$19 :",
"match : MATCH $$19 match_options action",
"filter_action_builtin : filter_action_builtin_nojunk",
"filter_action_builtin : JUNK",
"filter_action_builtin : BYPASS",
"filter_action_builtin_nojunk : REJECT STRING",
"filter_action_builtin_nojunk : DISCONNECT STRING",
"filter_action_builtin_nojunk : REWRITE STRING",
"filter_action_builtin_nojunk : REPORT STRING",
"filter_phase_check_fcrdns : negation FCRDNS",
"filter_phase_check_rdns : negation RDNS",
"filter_phase_check_rdns_table : negation RDNS tables",
"filter_phase_check_rdns_regex : negation RDNS REGEX tables",
"filter_phase_check_src_table : negation SRC tables",
"filter_phase_check_src_regex : negation SRC REGEX tables",
"filter_phase_check_helo_table : negation HELO tables",
"filter_phase_check_helo_regex : negation HELO REGEX tables",
"filter_phase_check_auth : negation AUTH",
"filter_phase_check_auth_table : negation AUTH tables",
"filter_phase_check_auth_regex : negation AUTH REGEX tables",
"filter_phase_check_mail_from_table : negation MAIL_FROM tables",
"filter_phase_check_mail_from_regex : negation MAIL_FROM REGEX tables",
"filter_phase_check_rcpt_to_table : negation RCPT_TO tables",
"filter_phase_check_rcpt_to_regex : negation RCPT_TO REGEX tables",
"filter_phase_global_options : filter_phase_check_fcrdns",
"filter_phase_global_options : filter_phase_check_rdns",
"filter_phase_global_options : filter_phase_check_rdns_regex",
"filter_phase_global_options : filter_phase_check_rdns_table",
"filter_phase_global_options : filter_phase_check_src_regex",
"filter_phase_global_options : filter_phase_check_src_table",
"filter_phase_connect_options : filter_phase_global_options",
"filter_phase_helo_options : filter_phase_check_helo_table",
"filter_phase_helo_options : filter_phase_check_helo_regex",
"filter_phase_helo_options : filter_phase_global_options",
"filter_phase_auth_options : filter_phase_check_helo_table",
"filter_phase_auth_options : filter_phase_check_helo_regex",
"filter_phase_auth_options : filter_phase_check_auth",
"filter_phase_auth_options : filter_phase_check_auth_table",
"filter_phase_auth_options : filter_phase_check_auth_regex",
"filter_phase_auth_options : filter_phase_global_options",
"filter_phase_mail_from_options : filter_phase_check_helo_table",
"filter_phase_mail_from_options : filter_phase_check_helo_regex",
"filter_phase_mail_from_options : filter_phase_check_auth",
"filter_phase_mail_from_options : filter_phase_check_auth_table",
"filter_phase_mail_from_options : filter_phase_check_auth_regex",
"filter_phase_mail_from_options : filter_phase_check_mail_from_table",
"filter_phase_mail_from_options : filter_phase_check_mail_from_regex",
"filter_phase_mail_from_options : filter_phase_global_options",
"filter_phase_rcpt_to_options : filter_phase_check_helo_table",
"filter_phase_rcpt_to_options : filter_phase_check_helo_regex",
"filter_phase_rcpt_to_options : filter_phase_check_auth",
"filter_phase_rcpt_to_options : filter_phase_check_auth_table",
"filter_phase_rcpt_to_options : filter_phase_check_auth_regex",
"filter_phase_rcpt_to_options : filter_phase_check_mail_from_table",
"filter_phase_rcpt_to_options : filter_phase_check_mail_from_regex",
"filter_phase_rcpt_to_options : filter_phase_check_rcpt_to_table",
"filter_phase_rcpt_to_options : filter_phase_check_rcpt_to_regex",
"filter_phase_rcpt_to_options : filter_phase_global_options",
"filter_phase_data_options : filter_phase_check_helo_table",
"filter_phase_data_options : filter_phase_check_helo_regex",
"filter_phase_data_options : filter_phase_check_auth",
"filter_phase_data_options : filter_phase_check_auth_table",
"filter_phase_data_options : filter_phase_check_auth_regex",
"filter_phase_data_options : filter_phase_check_mail_from_table",
"filter_phase_data_options : filter_phase_check_mail_from_regex",
"filter_phase_data_options : filter_phase_global_options",
"filter_phase_commit_options : filter_phase_check_helo_table",
"filter_phase_commit_options : filter_phase_check_helo_regex",
"filter_phase_commit_options : filter_phase_check_auth",
"filter_phase_commit_options : filter_phase_check_auth_table",
"filter_phase_commit_options : filter_phase_check_auth_regex",
"filter_phase_commit_options : filter_phase_check_mail_from_table",
"filter_phase_commit_options : filter_phase_check_mail_from_regex",
"filter_phase_commit_options : filter_phase_global_options",
"$$20 :",
"filter_phase_connect : CONNECT $$20 MATCH filter_phase_connect_options filter_action_builtin",
"$$21 :",
"filter_phase_helo : HELO $$21 MATCH filter_phase_helo_options filter_action_builtin",
"$$22 :",
"filter_phase_ehlo : EHLO $$22 MATCH filter_phase_helo_options filter_action_builtin",
"$$23 :",
"filter_phase_auth : AUTH $$23 MATCH filter_phase_auth_options filter_action_builtin",
"$$24 :",
"filter_phase_mail_from : MAIL_FROM $$24 MATCH filter_phase_mail_from_options filter_action_builtin",
"$$25 :",
"filter_phase_rcpt_to : RCPT_TO $$25 MATCH filter_phase_rcpt_to_options filter_action_builtin",
"$$26 :",
"filter_phase_data : DATA $$26 MATCH filter_phase_data_options filter_action_builtin",
"$$27 :",
"filter_phase_commit : COMMIT $$27 MATCH filter_phase_commit_options filter_action_builtin_nojunk",
"filter_phase : filter_phase_connect",
"filter_phase : filter_phase_helo",
"filter_phase : filter_phase_ehlo",
"filter_phase : filter_phase_auth",
"filter_phase : filter_phase_mail_from",
"filter_phase : filter_phase_rcpt_to",
"filter_phase : filter_phase_data",
"filter_phase : filter_phase_commit",
"filterel : STRING",
"filter_list : filterel optnl",
"filter_list : filterel comma filter_list",
"filter : FILTER STRING PROC STRING",
"$$28 :",
"filter : FILTER STRING PROC_EXEC STRING $$28 proc_params",
"$$29 :",
"filter : FILTER STRING PHASE $$29 filter_phase",
"$$30 :",
"filter : FILTER STRING CHAIN $$30 '{' optnl filter_list '}'",
"size : NUMBER",
"size : STRING",
"bouncedelay : STRING",
"bouncedelays : bouncedelays ',' bouncedelay",
"bouncedelays : bouncedelay",
"opt_limit_mda : STRING NUMBER",
"limits_smtp : opt_limit_smtp limits_smtp",
"limits_smtp :",
"opt_limit_smtp : STRING NUMBER",
"limits_mda : opt_limit_mda limits_mda",
"limits_mda :",
"opt_limit_mta : INET4",
"opt_limit_mta : INET6",
"opt_limit_mta : STRING NUMBER",
"limits_mta : opt_limit_mta limits_mta",
"limits_mta :",
"opt_limit_scheduler : STRING NUMBER",
"limits_scheduler : opt_limit_scheduler limits_scheduler",
"limits_scheduler :",
"opt_sock_listen : FILTER STRING",
"$$31 :",
"opt_sock_listen : FILTER $$31 '{' optnl filter_list '}'",
"opt_sock_listen : MASK_SRC",
"opt_sock_listen : NO_DSN",
"opt_sock_listen : TAG STRING",
"opt_if_listen : INET4",
"opt_if_listen : INET6",
"opt_if_listen : PORT STRING",
"opt_if_listen : PORT SMTP",
"opt_if_listen : PORT SMTPS",
"opt_if_listen : PORT NUMBER",
"opt_if_listen : FILTER STRING",
"$$32 :",
"opt_if_listen : FILTER $$32 '{' optnl filter_list '}'",
"opt_if_listen : SMTPS",
"opt_if_listen : SMTPS VERIFY",
"opt_if_listen : TLS",
"opt_if_listen : TLS_REQUIRE",
"opt_if_listen : TLS_REQUIRE VERIFY",
"opt_if_listen : CIPHERS STRING",
"opt_if_listen : PROTOCOLS STRING",
"opt_if_listen : PKI STRING",
"opt_if_listen : CA STRING",
"opt_if_listen : AUTH",
"opt_if_listen : AUTH_OPTIONAL",
"opt_if_listen : AUTH tables",
"opt_if_listen : AUTH_OPTIONAL tables",
"opt_if_listen : TAG STRING",
"opt_if_listen : HOSTNAME STRING",
"opt_if_listen : HOSTNAMES tables",
"opt_if_listen : MASK_SRC",
"opt_if_listen : RECEIVEDAUTH",
"opt_if_listen : NO_DSN",
"opt_if_listen : PROXY_V2",
"opt_if_listen : SENDERS tables",
"opt_if_listen : SENDERS tables MASQUERADE",
"listener_type : socket_listener",
"listener_type : if_listener",
"socket_listener : SOCKET sock_listen",
"if_listener : STRING if_listen",
"sock_listen : opt_sock_listen sock_listen",
"sock_listen :",
"if_listen : opt_if_listen if_listen",
"if_listen :",
"$$33 :",
"listen : LISTEN $$33 ON listener_type",
"table : TABLE STRING STRING",
"$$34 :",
"table : TABLE STRING $$34 '{' optnl tableval_list '}'",
"tablenew : STRING",
"$$35 :",
"tablenew : '{' optnl $$35 tableval_list '}'",
"tableref : '<' STRING '>'",
"tables : tablenew",
"tables : tableref",
};
#endif
#ifdef YYSTACKSIZE
#undef YYMAXDEPTH
#define YYMAXDEPTH YYSTACKSIZE
#else
#ifdef YYMAXDEPTH
#define YYSTACKSIZE YYMAXDEPTH
#else
#define YYSTACKSIZE 10000
#define YYMAXDEPTH 10000
#endif
#endif
#define YYINITSTACKSIZE 200
/* LINTUSED */
int yydebug;
int yynerrs;
int yyerrflag;
int yychar;
short *yyssp;
YYSTYPE *yyvsp;
YYSTYPE yyval;
YYSTYPE yylval;
short *yyss;
short *yysslim;
YYSTYPE *yyvs;
unsigned int yystacksize;
int yyparse(void);
#line 2628 "../../usr.sbin/smtpd/parse.y"

struct keywords {
	const char	*k_name;
	int		 k_val;
};

int
yyerror(const char *fmt, ...)
{
	va_list		 ap;
	char		*msg;

	file->errors++;
	va_start(ap, fmt);
	if (vasprintf(&msg, fmt, ap) == -1)
		fatalx("yyerror vasprintf");
	va_end(ap);
	logit(LOG_CRIT, "%s:%d: %s", file->name, yylval.lineno, msg);
	free(msg);
	return (0);
}

int
kw_cmp(const void *k, const void *e)
{
	return (strcmp(k, ((const struct keywords *)e)->k_name));
}

int
lookup(char *s)
{
	/* this has to be sorted always */
	static const struct keywords keywords[] = {
		{ "action",		ACTION },
		{ "admd",		ADMD },
		{ "alias",		ALIAS },
		{ "any",		ANY },
		{ "auth",		AUTH },
		{ "auth-optional",     	AUTH_OPTIONAL },
		{ "backup",		BACKUP },
		{ "bounce",		BOUNCE },
		{ "bypass",		BYPASS },
		{ "ca",			CA },
		{ "cert",		CERT },
		{ "chain",		CHAIN },
		{ "chroot",		CHROOT },
		{ "ciphers",		CIPHERS },
		{ "commit",		COMMIT },
		{ "compression",	COMPRESSION },
		{ "connect",		CONNECT },
		{ "data",		DATA },
		{ "data-line",		DATA_LINE },
		{ "dhe",		DHE },
		{ "disconnect",		DISCONNECT },
		{ "domain",		DOMAIN },
		{ "ehlo",		EHLO },
		{ "encryption",		ENCRYPTION },
		{ "expand-only",      	EXPAND_ONLY },
		{ "fcrdns",		FCRDNS },
		{ "filter",		FILTER },
		{ "for",		FOR },
		{ "forward-only",      	FORWARD_ONLY },
		{ "from",		FROM },
		{ "group",		GROUP },
		{ "helo",		HELO },
		{ "helo-src",       	HELO_SRC },
		{ "host",		HOST },
		{ "hostname",		HOSTNAME },
		{ "hostnames",		HOSTNAMES },
		{ "include",		INCLUDE },
		{ "inet4",		INET4 },
		{ "inet6",		INET6 },
		{ "junk",		JUNK },
		{ "key",		KEY },
		{ "limit",		LIMIT },
		{ "listen",		LISTEN },
		{ "lmtp",		LMTP },
		{ "local",		LOCAL },
		{ "mail-from",		MAIL_FROM },
		{ "maildir",		MAILDIR },
		{ "mask-src",		MASK_SRC },
		{ "masquerade",		MASQUERADE },
		{ "match",		MATCH },
		{ "max-deferred",  	MAX_DEFERRED },
		{ "max-message-size",  	MAX_MESSAGE_SIZE },
		{ "mbox",		MBOX },
		{ "mda",		MDA },
		{ "mta",		MTA },
		{ "mx",			MX },
		{ "no-dsn",		NO_DSN },
		{ "no-verify",		NO_VERIFY },
		{ "noop",		NOOP },
		{ "on",			ON },
		{ "phase",		PHASE },
		{ "pki",		PKI },
		{ "port",		PORT },
		{ "proc",		PROC },
		{ "proc-exec",		PROC_EXEC },
		{ "protocols",		PROTOCOLS },
		{ "proxy-v2",		PROXY_V2 },
		{ "queue",		QUEUE },
		{ "quit",		QUIT },
		{ "rcpt-to",		RCPT_TO },
		{ "rdns",		RDNS },
		{ "received-auth",     	RECEIVEDAUTH },
		{ "recipient",		RECIPIENT },
		{ "regex",		REGEX },
		{ "reject",		REJECT },
		{ "relay",		RELAY },
		{ "report",		REPORT },
		{ "rewrite",		REWRITE },
		{ "rset",		RSET },
		{ "scheduler",		SCHEDULER },
		{ "senders",   		SENDERS },
		{ "smtp",		SMTP },
		{ "smtp-in",		SMTP_IN },
		{ "smtp-out",		SMTP_OUT },
		{ "smtps",		SMTPS },
		{ "socket",		SOCKET },
		{ "src",		SRC },
		{ "srs",		SRS },
		{ "sub-addr-delim",	SUB_ADDR_DELIM },
		{ "table",		TABLE },
		{ "tag",		TAG },
		{ "tagged",		TAGGED },
		{ "tls",		TLS },
		{ "tls-require",       	TLS_REQUIRE },
		{ "ttl",		TTL },
		{ "user",		USER },
		{ "userbase",		USERBASE },
		{ "verify",		VERIFY },
		{ "virtual",		VIRTUAL },
		{ "warn-interval",	WARN_INTERVAL },
		{ "wrapper",		WRAPPER },
	};
	const struct keywords	*p;

	p = bsearch(s, keywords, sizeof(keywords)/sizeof(keywords[0]),
	    sizeof(keywords[0]), kw_cmp);

	if (p)
		return (p->k_val);
	else
		return (STRING);
}

#define START_EXPAND	1
#define DONE_EXPAND	2

static int	expanding;

int
igetc(void)
{
	int	c;

	while (1) {
		if (file->ungetpos > 0)
			c = file->ungetbuf[--file->ungetpos];
		else
			c = getc(file->stream);

		if (c == START_EXPAND)
			expanding = 1;
		else if (c == DONE_EXPAND)
			expanding = 0;
		else
			break;
	}
	return (c);
}

int
lgetc(int quotec)
{
	int		c, next;

	if (quotec) {
		if ((c = igetc()) == EOF) {
			yyerror("reached end of file while parsing "
			    "quoted string");
			if (file == topfile || popfile() == EOF)
				return (EOF);
			return (quotec);
		}
		return (c);
	}

	while ((c = igetc()) == '\\') {
		next = igetc();
		if (next != '\n') {
			c = next;
			break;
		}
		yylval.lineno = file->lineno;
		file->lineno++;
	}

	if (c == EOF) {
		/*
		 * Fake EOL when hit EOF for the first time. This gets line
		 * count right if last line in included file is syntactically
		 * invalid and has no newline.
		 */
		if (file->eof_reached == 0) {
			file->eof_reached = 1;
			return ('\n');
		}
		while (c == EOF) {
			if (file == topfile || popfile() == EOF)
				return (EOF);
			c = igetc();
		}
	}
	return (c);
}

void
lungetc(int c)
{
	if (c == EOF)
		return;

	if (file->ungetpos >= file->ungetsize) {
		void *p = reallocarray(file->ungetbuf, file->ungetsize, 2);
		if (p == NULL)
			fatal("%s", __func__);
		file->ungetbuf = p;
		file->ungetsize *= 2;
	}
	file->ungetbuf[file->ungetpos++] = c;
}

int
findeol(void)
{
	int	c;

	/* skip to either EOF or the first real EOL */
	while (1) {
		c = lgetc(0);
		if (c == '\n') {
			file->lineno++;
			break;
		}
		if (c == EOF)
			break;
	}
	return (ERROR);
}

int
yylex(void)
{
	char	 buf[8096];
	char	*p, *val;
	int	 quotec, next, c;
	int	 token;

top:
	p = buf;
	while ((c = lgetc(0)) == ' ' || c == '\t')
		; /* nothing */

	yylval.lineno = file->lineno;
	if (c == '#')
		while ((c = lgetc(0)) != '\n' && c != EOF)
			; /* nothing */
	if (c == '$' && !expanding) {
		while (1) {
			if ((c = lgetc(0)) == EOF)
				return (0);

			if (p + 1 >= buf + sizeof(buf) - 1) {
				yyerror("string too long");
				return (findeol());
			}
			if (isalnum(c) || c == '_') {
				*p++ = c;
				continue;
			}
			*p = '\0';
			lungetc(c);
			break;
		}
		val = symget(buf);
		if (val == NULL) {
			yyerror("macro '%s' not defined", buf);
			return (findeol());
		}
		p = val + strlen(val) - 1;
		lungetc(DONE_EXPAND);
		while (p >= val) {
			lungetc((unsigned char)*p);
			p--;
		}
		lungetc(START_EXPAND);
		goto top;
	}

	switch (c) {
	case '\'':
	case '"':
		quotec = c;
		while (1) {
			if ((c = lgetc(quotec)) == EOF)
				return (0);
			if (c == '\n') {
				file->lineno++;
				continue;
			} else if (c == '\\') {
				if ((next = lgetc(quotec)) == EOF)
					return (0);
				if (next == quotec || next == ' ' ||
				    next == '\t')
					c = next;
				else if (next == '\n') {
					file->lineno++;
					continue;
				} else
					lungetc(next);
			} else if (c == quotec) {
				*p = '\0';
				break;
			} else if (c == '\0') {
				yyerror("syntax error");
				return (findeol());
			}
			if (p + 1 >= buf + sizeof(buf) - 1) {
				yyerror("string too long");
				return (findeol());
			}
			*p++ = c;
		}
		yylval.v.string = strdup(buf);
		if (yylval.v.string == NULL)
			fatal("%s", __func__);
		return (STRING);
	}

#define allowed_to_end_number(x) \
	(isspace(x) || x == ')' || x ==',' || x == '/' || x == '}' || x == '=')

	if (c == '-' || isdigit(c)) {
		do {
			*p++ = c;
			if ((size_t)(p-buf) >= sizeof(buf)) {
				yyerror("string too long");
				return (findeol());
			}
		} while ((c = lgetc(0)) != EOF && isdigit(c));
		lungetc(c);
		if (p == buf + 1 && buf[0] == '-')
			goto nodigits;
		if (c == EOF || allowed_to_end_number(c)) {
			const char *errstr = NULL;

			*p = '\0';
			yylval.v.number = strtonum(buf, LLONG_MIN,
			    LLONG_MAX, &errstr);
			if (errstr) {
				yyerror("\"%s\" invalid number: %s",
				    buf, errstr);
				return (findeol());
			}
			return (NUMBER);
		} else {
nodigits:
			while (p > buf + 1)
				lungetc((unsigned char)*--p);
			c = (unsigned char)*--p;
			if (c == '-')
				return (c);
		}
	}

	if (c == '=') {
		if ((c = lgetc(0)) != EOF && c == '>')
			return (ARROW);
		lungetc(c);
		c = '=';
	}

#define allowed_in_string(x) \
	(isalnum(x) || (ispunct(x) && x != '(' && x != ')' && \
	x != '{' && x != '}' && x != '<' && x != '>' && \
	x != '!' && x != '=' && x != '#' && \
	x != ','))

	if (isalnum(c) || c == ':' || c == '_') {
		do {
			*p++ = c;
			if ((size_t)(p-buf) >= sizeof(buf)) {
				yyerror("string too long");
				return (findeol());
			}
		} while ((c = lgetc(0)) != EOF && (allowed_in_string(c)));
		lungetc(c);
		*p = '\0';
		if ((token = lookup(buf)) == STRING)
			if ((yylval.v.string = strdup(buf)) == NULL)
				fatal("%s", __func__);
		return (token);
	}
	if (c == '\n') {
		yylval.lineno = file->lineno;
		file->lineno++;
	}
	if (c == EOF)
		return (0);
	return (c);
}

int
check_file_secrecy(int fd, const char *fname)
{
	struct stat	st;

	if (fstat(fd, &st)) {
		log_warn("warn: cannot stat %s", fname);
		return (-1);
	}
	if (st.st_uid != 0 && st.st_uid != getuid()) {
		log_warnx("warn: %s: owner not root or current user", fname);
		return (-1);
	}
	if (st.st_mode & (S_IWGRP | S_IXGRP | S_IRWXO)) {
		log_warnx("warn: %s: group/world readable/writeable", fname);
		return (-1);
	}
	return (0);
}

struct file *
pushfile(const char *name, int secret)
{
	struct file	*nfile;

	if ((nfile = calloc(1, sizeof(struct file))) == NULL) {
		log_warn("%s", __func__);
		return (NULL);
	}
	if ((nfile->name = strdup(name)) == NULL) {
		log_warn("%s", __func__);
		free(nfile);
		return (NULL);
	}
	if ((nfile->stream = fopen(nfile->name, "r")) == NULL) {
		log_warn("%s: %s", __func__, nfile->name);
		free(nfile->name);
		free(nfile);
		return (NULL);
	} else if (secret &&
	    check_file_secrecy(fileno(nfile->stream), nfile->name)) {
		fclose(nfile->stream);
		free(nfile->name);
		free(nfile);
		return (NULL);
	}
	nfile->lineno = TAILQ_EMPTY(&files) ? 1 : 0;
	nfile->ungetsize = 16;
	nfile->ungetbuf = malloc(nfile->ungetsize);
	if (nfile->ungetbuf == NULL) {
		log_warn("%s", __func__);
		fclose(nfile->stream);
		free(nfile->name);
		free(nfile);
		return (NULL);
	}
	TAILQ_INSERT_TAIL(&files, nfile, entry);
	return (nfile);
}

int
popfile(void)
{
	struct file	*prev;

	if ((prev = TAILQ_PREV(file, files, entry)) != NULL)
		prev->errors += file->errors;

	TAILQ_REMOVE(&files, file, entry);
	fclose(file->stream);
	free(file->name);
	free(file->ungetbuf);
	free(file);
	file = prev;
	return (file ? 0 : EOF);
}

int
parse_config(struct smtpd *x_conf, const char *filename, int opts)
{
	struct sym     *sym, *next;

	conf = x_conf;
	errors = 0;

	if ((file = pushfile(filename, 0)) == NULL) {
		purge_config(PURGE_EVERYTHING);
		return (-1);
	}
	topfile = file;

	/*
	 * parse configuration
	 */
	setservent(1);
	yyparse();
	errors = file->errors;
	popfile();
	endservent();

	/* If the socket listener was not configured, create a default one. */
	if (!conf->sc_sock_listener) {
		memset(&listen_opts, 0, sizeof listen_opts);
		listen_opts.family = AF_UNSPEC;
		listen_opts.flags |= F_EXT_DSN;
		create_sock_listener(&listen_opts);
	}

	/* Free macros and check which have not been used. */
	TAILQ_FOREACH_SAFE(sym, &symhead, entry, next) {
		if ((conf->sc_opts & SMTPD_OPT_VERBOSE) && !sym->used)
			fprintf(stderr, "warning: macro '%s' not "
			    "used\n", sym->nam);
		if (!sym->persist) {
			free(sym->nam);
			free(sym->val);
			TAILQ_REMOVE(&symhead, sym, entry);
			free(sym);
		}
	}

	if (TAILQ_EMPTY(conf->sc_rules)) {
		log_warnx("warn: no rules, nothing to do");
		errors++;
	}

	if (errors) {
		purge_config(PURGE_EVERYTHING);
		return (-1);
	}

	return (0);
}

int
symset(const char *nam, const char *val, int persist)
{
	struct sym	*sym;

	TAILQ_FOREACH(sym, &symhead, entry) {
		if (strcmp(nam, sym->nam) == 0)
			break;
	}

	if (sym != NULL) {
		if (sym->persist == 1)
			return (0);
		else {
			free(sym->nam);
			free(sym->val);
			TAILQ_REMOVE(&symhead, sym, entry);
			free(sym);
		}
	}
	if ((sym = calloc(1, sizeof(*sym))) == NULL)
		return (-1);

	sym->nam = strdup(nam);
	if (sym->nam == NULL) {
		free(sym);
		return (-1);
	}
	sym->val = strdup(val);
	if (sym->val == NULL) {
		free(sym->nam);
		free(sym);
		return (-1);
	}
	sym->used = 0;
	sym->persist = persist;
	TAILQ_INSERT_TAIL(&symhead, sym, entry);
	return (0);
}

int
cmdline_symset(char *s)
{
	char	*sym, *val;
	int	ret;

	if ((val = strrchr(s, '=')) == NULL)
		return (-1);
	sym = strndup(s, val - s);
	if (sym == NULL)
		fatalx("%s: strndup", __func__);
	ret = symset(sym, val + 1, 1);
	free(sym);

	return (ret);
}

char *
symget(const char *nam)
{
	struct sym	*sym;

	TAILQ_FOREACH(sym, &symhead, entry) {
		if (strcmp(nam, sym->nam) == 0) {
			sym->used = 1;
			return (sym->val);
		}
	}
	return (NULL);
}

static void
create_sock_listener(struct listen_opts *lo)
{
	struct listener *l = xcalloc(1, sizeof(*l));
	lo->hostname = conf->sc_hostname;
	l->ss.ss_family = AF_LOCAL;
#ifdef HAVE_STRUCT_SOCKADDR_STORAGE_SS_LEN
	l->ss.ss_len = sizeof(struct sockaddr *);
#endif
	l->local = 1;
	conf->sc_sock_listener = l;
	config_listener(l, lo);
}

static void
create_if_listener(struct listen_opts *lo)
{
	uint16_t	flags;

	if (lo->port != 0 && lo->ssl == F_SSL)
		fatalx("invalid listen option: tls/smtps on same port");

	if (lo->auth != 0 && !lo->ssl)
		fatalx("invalid listen option: auth requires tls/smtps");

	if (lo->pkicount && !lo->ssl)
		fatalx("invalid listen option: pki requires tls/smtps");
	if (lo->pkicount == 0 && lo->ssl)
		fatalx("invalid listen option: pki required for tls/smtps");

	flags = lo->flags;

	if (lo->port) {
		lo->flags = lo->ssl|lo->auth|flags;
		lo->port = htons(lo->port);
	}
	else {
		if (lo->ssl & F_SMTPS) {
			lo->port = htons(465);
			lo->flags = F_SMTPS|lo->auth|flags;
		}

		if (!lo->ssl || (lo->ssl & F_STARTTLS)) {
			lo->port = htons(25);
			lo->flags = lo->auth|flags;
			if (lo->ssl & F_STARTTLS)
				lo->flags |= F_STARTTLS;
		}
	}

	if (interface(lo))
		return;
	if (host_v4(lo))
		return;
	if (host_v6(lo))
		return;
	if (host_dns(lo))
		return;

	fatalx("invalid virtual ip or interface: %s", lo->ifx);
}

static void
config_listener(struct listener *h,  struct listen_opts *lo)
{
	int i;

	h->fd = -1;
	h->port = lo->port;
	h->flags = lo->flags;

	if (lo->hostname == NULL)
		lo->hostname = conf->sc_hostname;

	if (lo->options & LO_FILTER) {
		h->flags |= F_FILTERED;
		(void)strlcpy(h->filter_name,
		    lo->filtername,
		    sizeof(h->filter_name));
	}

	if (lo->authtable != NULL)
		(void)strlcpy(h->authtable, lo->authtable->t_name, sizeof(h->authtable));

	h->pkicount = lo->pkicount;
	if (h->pkicount) {
		h->pki = calloc(h->pkicount, sizeof(*h->pki));
		if (h->pki == NULL)
			fatal("calloc");
	}
	for (i = 0; i < lo->pkicount; i++) {
		h->pki[i] = dict_get(conf->sc_pki_dict, lo->pki[i]);
		if (h->pki[i] == NULL) {
			log_warnx("pki name not found: %s", lo->pki[i]);
			fatalx(NULL);
		}
	}

	if (lo->tls_ciphers != NULL &&
	    (h->tls_ciphers = strdup(lo->tls_ciphers)) == NULL) {
		fatal("strdup");
	}

	if (lo->tls_protocols != NULL &&
	    (h->tls_protocols = strdup(lo->tls_protocols)) == NULL) {
		fatal("strdup");
	}

	if (lo->ca != NULL) {
		if (!lowercase(h->ca_name, lo->ca, sizeof(h->ca_name))) {
			log_warnx("ca name too long: %s", lo->ca);
			fatalx(NULL);
		}
		if (dict_get(conf->sc_ca_dict, h->ca_name) == NULL) {
			log_warnx("ca name not found: %s", lo->ca);
			fatalx(NULL);
		}
	}
	if (lo->tag != NULL)
		(void)strlcpy(h->tag, lo->tag, sizeof(h->tag));

	(void)strlcpy(h->hostname, lo->hostname, sizeof(h->hostname));
	if (lo->hostnametable)
		(void)strlcpy(h->hostnametable, lo->hostnametable->t_name, sizeof(h->hostnametable));
	if (lo->sendertable) {
		(void)strlcpy(h->sendertable, lo->sendertable->t_name, sizeof(h->sendertable));
		if (lo->options & LO_MASQUERADE)
			h->flags |= F_MASQUERADE;
	}

	if (lo->ssl & F_TLS_VERIFY)
		h->flags |= F_TLS_VERIFY;

	if (lo->ssl & F_STARTTLS_REQUIRE)
		h->flags |= F_STARTTLS_REQUIRE;
	
	if (h != conf->sc_sock_listener)
		TAILQ_INSERT_TAIL(conf->sc_listeners, h, entry);
}

static int
host_v4(struct listen_opts *lo)
{
	struct in_addr		 ina;
	struct sockaddr_in	*sain;
	struct listener		*h;

	if (lo->family != AF_UNSPEC && lo->family != AF_INET)
		return (0);

	memset(&ina, 0, sizeof(ina));
	if (inet_pton(AF_INET, lo->ifx, &ina) != 1)
		return (0);

	h = xcalloc(1, sizeof(*h));
	sain = (struct sockaddr_in *)&h->ss;
#ifdef HAVE_STRUCT_SOCKADDR_IN_SIN_LEN
	sain->sin_len = sizeof(struct sockaddr_in);
#endif
	sain->sin_family = AF_INET;
	sain->sin_addr.s_addr = ina.s_addr;
	sain->sin_port = lo->port;

	if (sain->sin_addr.s_addr == htonl(INADDR_LOOPBACK))
		h->local = 1;
	config_listener(h,  lo);

	return (1);
}

static int
host_v6(struct listen_opts *lo)
{
	struct in6_addr		 ina6;
	struct sockaddr_in6	*sin6;
	struct listener		*h;

	if (lo->family != AF_UNSPEC && lo->family != AF_INET6)
		return (0);

	memset(&ina6, 0, sizeof(ina6));
	if (inet_pton(AF_INET6, lo->ifx, &ina6) != 1)
		return (0);

	h = xcalloc(1, sizeof(*h));
	sin6 = (struct sockaddr_in6 *)&h->ss;
#ifdef HAVE_STRUCT_SOCKADDR_IN6_SIN6_LEN
	sin6->sin6_len = sizeof(struct sockaddr_in6);
#endif
	sin6->sin6_family = AF_INET6;
	sin6->sin6_port = lo->port;
	memcpy(&sin6->sin6_addr, &ina6, sizeof(ina6));

	if (IN6_IS_ADDR_LOOPBACK(&sin6->sin6_addr))
		h->local = 1;
	config_listener(h,  lo);

	return (1);
}

static int
host_dns(struct listen_opts *lo)
{
	struct addrinfo		 hints, *res0, *res;
	int			 error, cnt = 0;
	struct sockaddr_in	*sain;
	struct sockaddr_in6	*sin6;
	struct listener		*h;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = lo->family;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_ADDRCONFIG;
	error = getaddrinfo(lo->ifx, NULL, &hints, &res0);
	if (error == EAI_AGAIN || error == EAI_NODATA || error == EAI_NONAME)
		return (0);
	if (error) {
		log_warnx("warn: host_dns: could not parse \"%s\": %s", lo->ifx,
		    gai_strerror(error));
		return (-1);
	}

	for (res = res0; res; res = res->ai_next) {
		if (res->ai_family != AF_INET &&
		    res->ai_family != AF_INET6)
			continue;
		h = xcalloc(1, sizeof(*h));

		h->ss.ss_family = res->ai_family;
		if (res->ai_family == AF_INET) {
			sain = (struct sockaddr_in *)&h->ss;
#ifdef HAVE_STRUCT_SOCKADDR_IN_SIN_LEN
			sain->sin_len = sizeof(struct sockaddr_in);
#endif
			sain->sin_addr.s_addr = ((struct sockaddr_in *)
			    res->ai_addr)->sin_addr.s_addr;
			sain->sin_port = lo->port;
			if (sain->sin_addr.s_addr == htonl(INADDR_LOOPBACK))
				h->local = 1;
		} else {
			sin6 = (struct sockaddr_in6 *)&h->ss;
#ifdef HAVE_STRUCT_SOCKADDR_IN6_SIN6_LEN
			sin6->sin6_len = sizeof(struct sockaddr_in6);
#endif
			memcpy(&sin6->sin6_addr, &((struct sockaddr_in6 *)
			    res->ai_addr)->sin6_addr, sizeof(struct in6_addr));
			sin6->sin6_port = lo->port;
			if (IN6_IS_ADDR_LOOPBACK(&sin6->sin6_addr))
				h->local = 1;
		}

		config_listener(h, lo);

		cnt++;
	}

	freeaddrinfo(res0);
	return (cnt);
}

static int
interface(struct listen_opts *lo)
{
	struct ifaddrs *ifap, *p;
	struct sockaddr_in	*sain;
	struct sockaddr_in6	*sin6;
	struct listener		*h;
	int			ret = 0;

	if (getifaddrs(&ifap) == -1)
		fatal("getifaddrs");

	for (p = ifap; p != NULL; p = p->ifa_next) {
		if (p->ifa_addr == NULL)
			continue;
		if (strcmp(p->ifa_name, lo->ifx) != 0 &&
		    !is_if_in_group(p->ifa_name, lo->ifx))
			continue;
		if (lo->family != AF_UNSPEC && lo->family != p->ifa_addr->sa_family)
			continue;

		h = xcalloc(1, sizeof(*h));

		switch (p->ifa_addr->sa_family) {
		case AF_INET:
			sain = (struct sockaddr_in *)&h->ss;
			*sain = *(struct sockaddr_in *)p->ifa_addr;
#ifdef HAVE_STRUCT_SOCKADDR_IN_SIN_LEN
			sain->sin_len = sizeof(struct sockaddr_in);
#endif
			sain->sin_port = lo->port;
			if (sain->sin_addr.s_addr == htonl(INADDR_LOOPBACK))
				h->local = 1;
			break;

		case AF_INET6:
			sin6 = (struct sockaddr_in6 *)&h->ss;
			*sin6 = *(struct sockaddr_in6 *)p->ifa_addr;
#ifdef HAVE_STRUCT_SOCKADDR_IN6_SIN6_LEN
			sin6->sin6_len = sizeof(struct sockaddr_in6);
#endif
			sin6->sin6_port = lo->port;
#ifdef __KAME__
			if ((IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr) ||
			    IN6_IS_ADDR_MC_LINKLOCAL(&sin6->sin6_addr) ||
#ifdef USE_IPV6_ADDR_SCOPE_NODELOCAL
			    IN6_IS_ADDR_MC_NODELOCAL(&sin6->sin6_addr)) &&
#else
			    IN6_IS_ADDR_MC_INTFACELOCAL(&sin6->sin6_addr)) &&
#endif
			    sin6->sin6_scope_id == 0) {
				sin6->sin6_scope_id = ntohs(
				    *(u_int16_t *)&sin6->sin6_addr.s6_addr[2]);
				sin6->sin6_addr.s6_addr[2] = 0;
				sin6->sin6_addr.s6_addr[3] = 0;
			}
#endif
			if (IN6_IS_ADDR_LOOPBACK(&sin6->sin6_addr))
				h->local = 1;
			break;

		default:
			free(h);
			continue;
		}

		config_listener(h, lo);
		ret = 1;
	}

	freeifaddrs(ifap);

	return ret;
}

int
delaytonum(char *str)
{
	unsigned int     factor;
	size_t           len;
	const char      *errstr = NULL;
	int              delay;

	/* we need at least 1 digit and 1 unit */
	len = strlen(str);
	if (len < 2)
		goto bad;

	switch(str[len - 1]) {

	case 's':
		factor = 1;
		break;

	case 'm':
		factor = 60;
		break;

	case 'h':
		factor = 60 * 60;
		break;

	case 'd':
		factor = 24 * 60 * 60;
		break;

	default:
		goto bad;
	}

	str[len - 1] = '\0';
	delay = strtonum(str, 1, INT_MAX / factor, &errstr);
	if (errstr)
		goto bad;

	return (delay * factor);

bad:
	return (-1);
}

int
is_if_in_group(const char *ifname, const char *groupname)
{
#ifdef HAVE_STRUCT_IFGROUPREQ
        unsigned int		 len;
        struct ifgroupreq        ifgr;
        struct ifg_req          *ifg;
	int			 s;
	int			 ret = 0;

	if ((s = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
		fatal("socket");

        memset(&ifgr, 0, sizeof(ifgr));
        if (strlcpy(ifgr.ifgr_name, ifname, IFNAMSIZ) >= IFNAMSIZ)
		fatalx("interface name too large");

        if (ioctl(s, SIOCGIFGROUP, (caddr_t)&ifgr) == -1) {
                if (errno == EINVAL || errno == ENOTTY)
			goto end;
		fatal("SIOCGIFGROUP");
        }

        len = ifgr.ifgr_len;
        ifgr.ifgr_groups = xcalloc(len/sizeof(struct ifg_req),
		sizeof(struct ifg_req));
        if (ioctl(s, SIOCGIFGROUP, (caddr_t)&ifgr) == -1)
                fatal("SIOCGIFGROUP");

        ifg = ifgr.ifgr_groups;
        for (; ifg && len >= sizeof(struct ifg_req); ifg++) {
                len -= sizeof(struct ifg_req);
		if (strcmp(ifg->ifgrq_group, groupname) == 0) {
			ret = 1;
			break;
		}
        }
        free(ifgr.ifgr_groups);

end:
	close(s);
	return ret;
#else
	return (0);
#endif
}

static int
config_lo_mask_source(struct listen_opts *lo) {
	if (lo->options & LO_MASKSOURCE) {
		yyerror("mask-source already specified");
		return -1;
	}
	lo->options |= LO_MASKSOURCE;
	lo->flags |= F_MASK_SOURCE;

	return 0;
}

#line 2215 "../../usr.sbin/smtpd/parse.c"
/* allocate initial stack or double stack size, up to YYMAXDEPTH */
static int yygrowstack(void)
{
    unsigned int newsize;
    long sslen;
    short *newss;
    YYSTYPE *newvs;

    if ((newsize = yystacksize) == 0)
        newsize = YYINITSTACKSIZE;
    else if (newsize >= YYMAXDEPTH)
        return -1;
    else if ((newsize *= 2) > YYMAXDEPTH)
        newsize = YYMAXDEPTH;
    sslen = yyssp - yyss;
#ifdef SIZE_MAX
#define YY_SIZE_MAX SIZE_MAX
#else
#define YY_SIZE_MAX 0xffffffffU
#endif
    if (newsize && YY_SIZE_MAX / newsize < sizeof *newss)
        goto bail;
    newss = (short *)realloc(yyss, newsize * sizeof *newss);
    if (newss == NULL)
        goto bail;
    yyss = newss;
    yyssp = newss + sslen;
    if (newsize && YY_SIZE_MAX / newsize < sizeof *newvs)
        goto bail;
    newvs = (YYSTYPE *)realloc(yyvs, newsize * sizeof *newvs);
    if (newvs == NULL)
        goto bail;
    yyvs = newvs;
    yyvsp = newvs + sslen;
    yystacksize = newsize;
    yysslim = yyss + newsize - 1;
    return 0;
bail:
    if (yyss)
            free(yyss);
    if (yyvs)
            free(yyvs);
    yyss = yyssp = NULL;
    yyvs = yyvsp = NULL;
    yystacksize = 0;
    return -1;
}

#define YYABORT goto yyabort
#define YYREJECT goto yyabort
#define YYACCEPT goto yyaccept
#define YYERROR goto yyerrlab
int
yyparse(void)
{
    int yym, yyn, yystate;
#if YYDEBUG
    const char *yys;

    if ((yys = getenv("YYDEBUG")))
    {
        yyn = *yys;
        if (yyn >= '0' && yyn <= '9')
            yydebug = yyn - '0';
    }
#endif /* YYDEBUG */

    yynerrs = 0;
    yyerrflag = 0;
    yychar = (-1);

    if (yyss == NULL && yygrowstack()) goto yyoverflow;
    yyssp = yyss;
    yyvsp = yyvs;
    *yyssp = yystate = 0;

yyloop:
    if ((yyn = yydefred[yystate]) != 0) goto yyreduce;
    if (yychar < 0)
    {
        if ((yychar = yylex()) < 0) yychar = 0;
#if YYDEBUG
        if (yydebug)
        {
            yys = 0;
            if (yychar <= YYMAXTOKEN) yys = yyname[yychar];
            if (!yys) yys = "illegal-symbol";
            printf("%sdebug: state %d, reading %d (%s)\n",
                    YYPREFIX, yystate, yychar, yys);
        }
#endif
    }
    if ((yyn = yysindex[yystate]) && (yyn += yychar) >= 0 &&
            yyn <= YYTABLESIZE && yycheck[yyn] == yychar)
    {
#if YYDEBUG
        if (yydebug)
            printf("%sdebug: state %d, shifting to state %d\n",
                    YYPREFIX, yystate, yytable[yyn]);
#endif
        if (yyssp >= yysslim && yygrowstack())
        {
            goto yyoverflow;
        }
        *++yyssp = yystate = yytable[yyn];
        *++yyvsp = yylval;
        yychar = (-1);
        if (yyerrflag > 0)  --yyerrflag;
        goto yyloop;
    }
    if ((yyn = yyrindex[yystate]) && (yyn += yychar) >= 0 &&
            yyn <= YYTABLESIZE && yycheck[yyn] == yychar)
    {
        yyn = yytable[yyn];
        goto yyreduce;
    }
    if (yyerrflag) goto yyinrecovery;
#if defined(__GNUC__)
    goto yynewerror;
#endif
yynewerror:
    yyerror("syntax error");
#if defined(__GNUC__)
    goto yyerrlab;
#endif
yyerrlab:
    ++yynerrs;
yyinrecovery:
    if (yyerrflag < 3)
    {
        yyerrflag = 3;
        for (;;)
        {
            if ((yyn = yysindex[*yyssp]) && (yyn += YYERRCODE) >= 0 &&
                    yyn <= YYTABLESIZE && yycheck[yyn] == YYERRCODE)
            {
#if YYDEBUG
                if (yydebug)
                    printf("%sdebug: state %d, error recovery shifting\
 to state %d\n", YYPREFIX, *yyssp, yytable[yyn]);
#endif
                if (yyssp >= yysslim && yygrowstack())
                {
                    goto yyoverflow;
                }
                *++yyssp = yystate = yytable[yyn];
                *++yyvsp = yylval;
                goto yyloop;
            }
            else
            {
#if YYDEBUG
                if (yydebug)
                    printf("%sdebug: error recovery discarding state %d\n",
                            YYPREFIX, *yyssp);
#endif
                if (yyssp <= yyss) goto yyabort;
                --yyssp;
                --yyvsp;
            }
        }
    }
    else
    {
        if (yychar == 0) goto yyabort;
#if YYDEBUG
        if (yydebug)
        {
            yys = 0;
            if (yychar <= YYMAXTOKEN) yys = yyname[yychar];
            if (!yys) yys = "illegal-symbol";
            printf("%sdebug: state %d, error recovery discards token %d (%s)\n",
                    YYPREFIX, yystate, yychar, yys);
        }
#endif
        yychar = (-1);
        goto yyloop;
    }
yyreduce:
#if YYDEBUG
    if (yydebug)
        printf("%sdebug: state %d, reducing by rule %d (%s)\n",
                YYPREFIX, yystate, yyn, yyrule[yyn]);
#endif
    yym = yylen[yyn];
    if (yym)
        yyval = yyvsp[1-yym];
    else
        memset(&yyval, 0, sizeof yyval);
    switch (yyn)
    {
case 21:
#line 231 "../../usr.sbin/smtpd/parse.y"
{ file->errors++; }
break;
case 22:
#line 234 "../../usr.sbin/smtpd/parse.y"
{
			struct file	*nfile;

			if ((nfile = pushfile(yyvsp[0].v.string, 0)) == NULL) {
				yyerror("failed to include file %s", yyvsp[0].v.string);
				free(yyvsp[0].v.string);
				YYERROR;
			}
			free(yyvsp[0].v.string);

			file = nfile;
			lungetc('\n');
		}
break;
case 23:
#line 249 "../../usr.sbin/smtpd/parse.y"
{
			char *s = yyvsp[-2].v.string;
			while (*s++) {
				if (isspace((unsigned char)*s)) {
					yyerror("macro name cannot contain "
					    "whitespace");
					free(yyvsp[-2].v.string);
					free(yyvsp[0].v.string);
					YYERROR;
				}
			}
			if (symset(yyvsp[-2].v.string, yyvsp[0].v.string, 0) == -1)
				fatal("cannot store variable");
			free(yyvsp[-2].v.string);
			free(yyvsp[0].v.string);
		}
break;
case 30:
#line 279 "../../usr.sbin/smtpd/parse.y"
{ yyval.v.number = 1; }
break;
case 31:
#line 280 "../../usr.sbin/smtpd/parse.y"
{ yyval.v.number = 0; }
break;
case 34:
#line 286 "../../usr.sbin/smtpd/parse.y"
{
			table_add(table, yyvsp[-2].v.string, yyvsp[0].v.string);
			free(yyvsp[-2].v.string);
			free(yyvsp[0].v.string);
		}
break;
case 37:
#line 297 "../../usr.sbin/smtpd/parse.y"
{
			table_add(table, yyvsp[0].v.string, NULL);
			free(yyvsp[0].v.string);
		}
break;
case 40:
#line 307 "../../usr.sbin/smtpd/parse.y"
{ }
break;
case 41:
#line 308 "../../usr.sbin/smtpd/parse.y"
{ }
break;
case 42:
#line 312 "../../usr.sbin/smtpd/parse.y"
{
	memset(conf->sc_bounce_warn, 0, sizeof conf->sc_bounce_warn);
}
break;
case 44:
#line 319 "../../usr.sbin/smtpd/parse.y"
{
	size_t i;

	for (i = 0; yyvsp[0].v.string[i] != '\0'; i++) {
		if (!isprint(yyvsp[0].v.string[i])) {
			yyerror("not a valid admd");
			free(yyvsp[0].v.string);
			YYERROR;
		}
	}
	conf->sc_admd = yyvsp[0].v.string;
}
break;
case 45:
#line 334 "../../usr.sbin/smtpd/parse.y"
{
	char buf[HOST_NAME_MAX+1];

	/* if not catchall, check that it is a valid domain */
	if (strcmp(yyvsp[0].v.string, "*") != 0) {
		if (!res_hnok(yyvsp[0].v.string)) {
			yyerror("not a valid domain name: %s", yyvsp[0].v.string);
			free(yyvsp[0].v.string);
			YYERROR;
		}
	}
	xlowercase(buf, yyvsp[0].v.string, sizeof(buf));
	free(yyvsp[0].v.string);
	sca = dict_get(conf->sc_ca_dict, buf);
	if (sca == NULL) {
		sca = xcalloc(1, sizeof *sca);
		(void)strlcpy(sca->ca_name, buf, sizeof(sca->ca_name));
		dict_set(conf->sc_ca_dict, sca->ca_name, sca);
	}
}
break;
case 47:
#line 358 "../../usr.sbin/smtpd/parse.y"
{
	sca->ca_cert_file = yyvsp[0].v.string;
}
break;
case 50:
#line 370 "../../usr.sbin/smtpd/parse.y"
{
	if (dict_get(conf->sc_mda_wrappers, yyvsp[-1].v.string)) {
		yyerror("mda wrapper already declared with that name: %s", yyvsp[-1].v.string);
		YYERROR;
	}
	dict_set(conf->sc_mda_wrappers, yyvsp[-1].v.string, yyvsp[0].v.string);
}
break;
case 51:
#line 381 "../../usr.sbin/smtpd/parse.y"
{
	conf->sc_mta_max_deferred = yyvsp[0].v.number;
}
break;
case 52:
#line 384 "../../usr.sbin/smtpd/parse.y"
{
	struct mta_limits	*d;

	limits = dict_get(conf->sc_limits_dict, yyvsp[0].v.string);
	if (limits == NULL) {
		limits = xcalloc(1, sizeof(*limits));
		dict_xset(conf->sc_limits_dict, yyvsp[0].v.string, limits);
		d = dict_xget(conf->sc_limits_dict, "default");
		memmove(limits, d, sizeof(*limits));
	}
	free(yyvsp[0].v.string);
}
break;
case 54:
#line 396 "../../usr.sbin/smtpd/parse.y"
{
	limits = dict_get(conf->sc_limits_dict, "default");
}
break;
case 56:
#line 403 "../../usr.sbin/smtpd/parse.y"
{
	char buf[HOST_NAME_MAX+1];

	/* if not catchall, check that it is a valid domain */
	if (strcmp(yyvsp[0].v.string, "*") != 0) {
		if (!res_hnok(yyvsp[0].v.string)) {
			yyerror("not a valid domain name: %s", yyvsp[0].v.string);
			free(yyvsp[0].v.string);
			YYERROR;
		}
	}
	xlowercase(buf, yyvsp[0].v.string, sizeof(buf));
	free(yyvsp[0].v.string);
	pki = dict_get(conf->sc_pki_dict, buf);
	if (pki == NULL) {
		pki = xcalloc(1, sizeof *pki);
		(void)strlcpy(pki->pki_name, buf, sizeof(pki->pki_name));
		dict_set(conf->sc_pki_dict, pki->pki_name, pki);
	}
}
break;
case 58:
#line 426 "../../usr.sbin/smtpd/parse.y"
{
	pki->pki_cert_file = yyvsp[0].v.string;
}
break;
case 59:
#line 429 "../../usr.sbin/smtpd/parse.y"
{
	pki->pki_key_file = yyvsp[0].v.string;
}
break;
case 60:
#line 432 "../../usr.sbin/smtpd/parse.y"
{
	if (strcasecmp(yyvsp[0].v.string, "none") == 0)
		pki->pki_dhe = 0;
	else if (strcasecmp(yyvsp[0].v.string, "auto") == 0)
		pki->pki_dhe = 1;
	else if (strcasecmp(yyvsp[0].v.string, "legacy") == 0)
		pki->pki_dhe = 2;
	else {
		yyerror("invalid DHE keyword: %s", yyvsp[0].v.string);
		free(yyvsp[0].v.string);
		YYERROR;
	}
	free(yyvsp[0].v.string);
}
break;
case 63:
#line 456 "../../usr.sbin/smtpd/parse.y"
{
	if (dict_get(conf->sc_filter_processes_dict, yyvsp[-1].v.string)) {
		yyerror("processor already exists with that name: %s", yyvsp[-1].v.string);
		free(yyvsp[-1].v.string);
		free(yyvsp[0].v.string);
		YYERROR;
	}
	processor = xcalloc(1, sizeof *processor);
	processor->command = yyvsp[0].v.string;
}
break;
case 64:
#line 465 "../../usr.sbin/smtpd/parse.y"
{
	dict_set(conf->sc_filter_processes_dict, yyvsp[-3].v.string, processor);
	processor = NULL;
}
break;
case 65:
#line 473 "../../usr.sbin/smtpd/parse.y"
{
	if (processor->user) {
		yyerror("user already specified for this processor");
		free(yyvsp[0].v.string);
		YYERROR;
	}
	processor->user = yyvsp[0].v.string;
}
break;
case 66:
#line 481 "../../usr.sbin/smtpd/parse.y"
{
	if (processor->group) {
		yyerror("group already specified for this processor");
		free(yyvsp[0].v.string);
		YYERROR;
	}
	processor->group = yyvsp[0].v.string;
}
break;
case 67:
#line 489 "../../usr.sbin/smtpd/parse.y"
{
	if (processor->chroot) {
		yyerror("chroot already specified for this processor");
		free(yyvsp[0].v.string);
		YYERROR;
	}
	processor->chroot = yyvsp[0].v.string;
}
break;
case 70:
#line 506 "../../usr.sbin/smtpd/parse.y"
{
	conf->sc_queue_flags |= QUEUE_COMPRESSION;
}
break;
case 71:
#line 509 "../../usr.sbin/smtpd/parse.y"
{
	conf->sc_queue_flags |= QUEUE_ENCRYPTION;
}
break;
case 72:
#line 512 "../../usr.sbin/smtpd/parse.y"
{
	if (strcasecmp(yyvsp[0].v.string, "stdin") == 0 || strcasecmp(yyvsp[0].v.string, "-") == 0) {
		conf->sc_queue_key = "stdin";
		free(yyvsp[0].v.string);
	}
	else
		conf->sc_queue_key = yyvsp[0].v.string;
	conf->sc_queue_flags |= QUEUE_ENCRYPTION;
}
break;
case 73:
#line 521 "../../usr.sbin/smtpd/parse.y"
{
	conf->sc_ttl = delaytonum(yyvsp[0].v.string);
	if (conf->sc_ttl == -1) {
		yyerror("invalid ttl delay: %s", yyvsp[0].v.string);
		free(yyvsp[0].v.string);
		YYERROR;
	}
	free(yyvsp[0].v.string);
}
break;
case 76:
#line 540 "../../usr.sbin/smtpd/parse.y"
{
	conf->sc_tls_ciphers = yyvsp[0].v.string;
}
break;
case 77:
#line 543 "../../usr.sbin/smtpd/parse.y"
{
	conf->sc_maxsize = yyvsp[0].v.number;
}
break;
case 78:
#line 546 "../../usr.sbin/smtpd/parse.y"
{
	if (strlen(yyvsp[0].v.string) != 1) {
		yyerror("subaddressing-delimiter must be one character");
		free(yyvsp[0].v.string);
		YYERROR;
	}
	if (isspace((unsigned char)*yyvsp[0].v.string) || !isprint((unsigned char)*yyvsp[0].v.string) || *yyvsp[0].v.string == '@') {
		yyerror("sub-addr-delim uses invalid character");
		free(yyvsp[0].v.string);
		YYERROR;
	}
	conf->sc_subaddressing_delim = yyvsp[0].v.string;
}
break;
case 79:
#line 562 "../../usr.sbin/smtpd/parse.y"
{
	conf->sc_srs_key = yyvsp[0].v.string;
}
break;
case 80:
#line 565 "../../usr.sbin/smtpd/parse.y"
{
	conf->sc_srs_key_backup = yyvsp[0].v.string;
}
break;
case 81:
#line 568 "../../usr.sbin/smtpd/parse.y"
{
	conf->sc_srs_ttl = delaytonum(yyvsp[0].v.string);
	if (conf->sc_srs_ttl == -1) {
		yyerror("ttl delay \"%s\" is invalid", yyvsp[0].v.string);
		free(yyvsp[0].v.string);
		YYERROR;
	}

	conf->sc_srs_ttl /= 86400;
	if (conf->sc_srs_ttl == 0) {
		yyerror("ttl delay \"%s\" is too short", yyvsp[0].v.string);
		free(yyvsp[0].v.string);
		YYERROR;
	}
	free(yyvsp[0].v.string);
}
break;
case 82:
#line 588 "../../usr.sbin/smtpd/parse.y"
{
	if (dsp->u.local.is_mbox) {
		yyerror("user may not be specified for this dispatcher");
		YYERROR;
	}

	if (dsp->u.local.forward_only) {
		yyerror("user may not be specified for forward-only");
		YYERROR;
	}

	if (dsp->u.local.expand_only) {
		yyerror("user may not be specified for expand-only");
		YYERROR;
	}

	if (dsp->u.local.user) {
		yyerror("user already specified for this dispatcher");
		YYERROR;
	}

	dsp->u.local.user = yyvsp[0].v.string;
}
break;
case 83:
#line 611 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = yyvsp[0].v.table;

	if (dsp->u.local.table_alias) {
		yyerror("alias mapping already specified for this dispatcher");
		YYERROR;
	}

	if (dsp->u.local.table_virtual) {
		yyerror("virtual mapping already specified for this dispatcher");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_HASH, K_ALIAS)) {
		yyerror("table \"%s\" may not be used for alias lookups",
		    t->t_name);
		YYERROR;
	}

	dsp->u.local.table_alias = strdup(t->t_name);
}
break;
case 84:
#line 632 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = yyvsp[0].v.table;

	if (dsp->u.local.table_virtual) {
		yyerror("virtual mapping already specified for this dispatcher");
		YYERROR;
	}

	if (dsp->u.local.table_alias) {
		yyerror("alias mapping already specified for this dispatcher");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_HASH, K_ALIAS)) {
		yyerror("table \"%s\" may not be used for virtual lookups",
		    t->t_name);
		YYERROR;
	}

	dsp->u.local.table_virtual = strdup(t->t_name);
}
break;
case 85:
#line 653 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = yyvsp[0].v.table;

	if (dsp->u.local.table_userbase) {
		yyerror("userbase mapping already specified for this dispatcher");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_HASH, K_USERINFO)) {
		yyerror("table \"%s\" may not be used for userbase lookups",
		    t->t_name);
		YYERROR;
	}

	dsp->u.local.table_userbase = strdup(t->t_name);
}
break;
case 86:
#line 669 "../../usr.sbin/smtpd/parse.y"
{
	if (! dict_get(conf->sc_mda_wrappers, yyvsp[0].v.string)) {
		yyerror("no mda wrapper with that name: %s", yyvsp[0].v.string);
		YYERROR;
	}
	dsp->u.local.mda_wrapper = yyvsp[0].v.string;
}
break;
case 89:
#line 684 "../../usr.sbin/smtpd/parse.y"
{
	dsp->u.local.is_mbox = 1;
}
break;
case 91:
#line 687 "../../usr.sbin/smtpd/parse.y"
{
	asprintf(&dsp->u.local.command, PATH_LIBEXEC"/mail.maildir");
}
break;
case 93:
#line 690 "../../usr.sbin/smtpd/parse.y"
{
	asprintf(&dsp->u.local.command, PATH_LIBEXEC"/mail.maildir -j");
}
break;
case 95:
#line 693 "../../usr.sbin/smtpd/parse.y"
{
	if (strncmp(yyvsp[0].v.string, "~/", 2) == 0)
		asprintf(&dsp->u.local.command,
		    PATH_LIBEXEC"/mail.maildir \"%%{user.directory}/%s\"", yyvsp[0].v.string+2);
	else
		asprintf(&dsp->u.local.command,
		    PATH_LIBEXEC"/mail.maildir \"%s\"", yyvsp[0].v.string);
}
break;
case 97:
#line 701 "../../usr.sbin/smtpd/parse.y"
{
	if (strncmp(yyvsp[-1].v.string, "~/", 2) == 0)
		asprintf(&dsp->u.local.command,
		    PATH_LIBEXEC"/mail.maildir -j \"%%{user.directory}/%s\"", yyvsp[-1].v.string+2);
	else
		asprintf(&dsp->u.local.command,
		    PATH_LIBEXEC"/mail.maildir -j \"%s\"", yyvsp[-1].v.string);
}
break;
case 99:
#line 709 "../../usr.sbin/smtpd/parse.y"
{
	asprintf(&dsp->u.local.command,
	    PATH_LIBEXEC"/mail.lmtp -d %s -u", yyvsp[0].v.string);
}
break;
case 101:
#line 713 "../../usr.sbin/smtpd/parse.y"
{
	asprintf(&dsp->u.local.command,
	    PATH_LIBEXEC"/mail.lmtp -d %s -r", yyvsp[-1].v.string);
}
break;
case 103:
#line 717 "../../usr.sbin/smtpd/parse.y"
{
	asprintf(&dsp->u.local.command,
	    PATH_LIBEXEC"/mail.mda \"%s\"", yyvsp[0].v.string);
}
break;
case 105:
#line 721 "../../usr.sbin/smtpd/parse.y"
{
	dsp->u.local.forward_only = 1;
}
break;
case 107:
#line 724 "../../usr.sbin/smtpd/parse.y"
{
	dsp->u.local.expand_only = 1;
}
break;
case 109:
#line 731 "../../usr.sbin/smtpd/parse.y"
{
	if (dsp->u.remote.helo) {
		yyerror("helo already specified for this dispatcher");
		YYERROR;
	}

	dsp->u.remote.helo = yyvsp[0].v.string;
}
break;
case 110:
#line 739 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = yyvsp[0].v.table;

	if (dsp->u.remote.helo_source) {
		yyerror("helo-source mapping already specified for this dispatcher");
		YYERROR;
	}
	if (!table_check_use(t, T_DYNAMIC|T_HASH, K_ADDRNAME)) {
		yyerror("table \"%s\" may not be used for helo-source lookups",
		    t->t_name);
		YYERROR;
	}

	dsp->u.remote.helo_source = strdup(t->t_name);
}
break;
case 111:
#line 754 "../../usr.sbin/smtpd/parse.y"
{
	if (dsp->u.remote.pki) {
		yyerror("pki already specified for this dispatcher");
		YYERROR;
	}

	dsp->u.remote.pki = yyvsp[0].v.string;
}
break;
case 112:
#line 762 "../../usr.sbin/smtpd/parse.y"
{
	if (dsp->u.remote.ca) {
		yyerror("ca already specified for this dispatcher");
		YYERROR;
	}

	dsp->u.remote.ca = yyvsp[0].v.string;
}
break;
case 113:
#line 770 "../../usr.sbin/smtpd/parse.y"
{
	if (dsp->u.remote.tls_ciphers) {
		yyerror("ciphers already specified for this dispatcher");
		YYERROR;
	}

	dsp->u.remote.tls_ciphers = yyvsp[0].v.string;
}
break;
case 114:
#line 778 "../../usr.sbin/smtpd/parse.y"
{
	if (dsp->u.remote.tls_protocols) {
		yyerror("protocols already specified for this dispatcher");
		YYERROR;
	}

	dsp->u.remote.tls_protocols = yyvsp[0].v.string;
}
break;
case 115:
#line 786 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = yyvsp[0].v.table;

	if (dsp->u.remote.source) {
		yyerror("source mapping already specified for this dispatcher");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_LIST|T_HASH, K_SOURCE)) {
		yyerror("table \"%s\" may not be used for source lookups",
		    t->t_name);
		YYERROR;
	}

	dsp->u.remote.source = strdup(t->t_name);
}
break;
case 116:
#line 802 "../../usr.sbin/smtpd/parse.y"
{
	if (dsp->u.remote.mail_from) {
		yyerror("mail-from already specified for this dispatcher");
		YYERROR;
	}

	dsp->u.remote.mail_from = yyvsp[0].v.string;
}
break;
case 117:
#line 810 "../../usr.sbin/smtpd/parse.y"
{
	if (dsp->u.remote.backup) {
		yyerror("backup already specified for this dispatcher");
		YYERROR;
	}
	if (dsp->u.remote.smarthost) {
		yyerror("backup and host are mutually exclusive");
		YYERROR;
	}

	dsp->u.remote.backup = 1;
	dsp->u.remote.backupmx = yyvsp[0].v.string;
}
break;
case 118:
#line 823 "../../usr.sbin/smtpd/parse.y"
{
	if (dsp->u.remote.backup) {
		yyerror("backup already specified for this dispatcher");
		YYERROR;
	}
	if (dsp->u.remote.smarthost) {
		yyerror("backup and host are mutually exclusive");
		YYERROR;
	}

	dsp->u.remote.backup = 1;
}
break;
case 119:
#line 835 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = yyvsp[0].v.table;

	if (dsp->u.remote.smarthost) {
		yyerror("host mapping already specified for this dispatcher");
		YYERROR;
	}
	if (dsp->u.remote.backup) {
		yyerror("backup and host are mutually exclusive");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_LIST, K_RELAYHOST)) {
		yyerror("table \"%s\" may not be used for host lookups",
		    t->t_name);
		YYERROR;
	}

	dsp->u.remote.smarthost = strdup(t->t_name);
}
break;
case 120:
#line 855 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = yyvsp[0].v.table;

	if (dsp->u.remote.smarthost) {
		yyerror("host mapping already specified for this dispatcher");
		YYERROR;
	}
	if (dsp->u.remote.backup) {
		yyerror("backup and domain are mutually exclusive");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_HASH, K_RELAYHOST)) {
		yyerror("table \"%s\" may not be used for host lookups",
		    t->t_name);
		YYERROR;
	}

	dsp->u.remote.smarthost = strdup(t->t_name);
	dsp->u.remote.smarthost_domain = 1;
}
break;
case 121:
#line 876 "../../usr.sbin/smtpd/parse.y"
{
	if (dsp->u.remote.tls_required == 1) {
		yyerror("tls already specified for this dispatcher");
		YYERROR;
	}

	dsp->u.remote.tls_required = 1;
	dsp->u.remote.tls_verify = 1;
}
break;
case 122:
#line 885 "../../usr.sbin/smtpd/parse.y"
{
	if (dsp->u.remote.tls_required == 1) {
		yyerror("tls already specified for this dispatcher");
		YYERROR;
	}

	dsp->u.remote.tls_required = 1;
}
break;
case 123:
#line 893 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = yyvsp[0].v.table;

	if (dsp->u.remote.smarthost == NULL) {
		yyerror("auth may not be specified without host on a dispatcher");
		YYERROR;
	}

	if (dsp->u.remote.auth) {
		yyerror("auth mapping already specified for this dispatcher");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_HASH, K_CREDENTIALS)) {
		yyerror("table \"%s\" may not be used for auth lookups",
		    t->t_name);
		YYERROR;
	}

	dsp->u.remote.auth = strdup(t->t_name);
}
break;
case 124:
#line 914 "../../usr.sbin/smtpd/parse.y"
{
	struct filter_config *fc;

	if (dsp->u.remote.filtername) {
		yyerror("filter already specified for this dispatcher");
		YYERROR;
	}

	if ((fc = dict_get(conf->sc_filters_dict, yyvsp[0].v.string)) == NULL) {
		yyerror("no filter exist with that name: %s", yyvsp[0].v.string);
		free(yyvsp[0].v.string);
		YYERROR;
	}
	fc->filter_subsystem |= FILTER_SUBSYSTEM_SMTP_OUT;
	dsp->u.remote.filtername = yyvsp[0].v.string;
}
break;
case 125:
#line 930 "../../usr.sbin/smtpd/parse.y"
{
	char	buffer[128];
	char	*filtername;

	if (dsp->u.remote.filtername) {
		yyerror("filter already specified for this dispatcher");
		YYERROR;
	}

	do {
		(void)snprintf(buffer, sizeof buffer, "<dynchain:%08x>", last_dynchain_id++);
	} while (dict_check(conf->sc_filters_dict, buffer));

	filtername = xstrdup(buffer);
	filter_config = xcalloc(1, sizeof *filter_config);
	filter_config->filter_type = FILTER_TYPE_CHAIN;
	filter_config->filter_subsystem |= FILTER_SUBSYSTEM_SMTP_OUT;
	dict_init(&filter_config->chain_procs);
	dsp->u.remote.filtername = filtername;
}
break;
case 126:
#line 949 "../../usr.sbin/smtpd/parse.y"
{
	dict_set(conf->sc_filters_dict, dsp->u.remote.filtername, filter_config);
	filter_config = NULL;
}
break;
case 127:
#line 953 "../../usr.sbin/smtpd/parse.y"
{
	if (conf->sc_srs_key == NULL) {
		yyerror("an srs key is required for srs to be specified in an action");
		YYERROR;
	}
	if (dsp->u.remote.srs == 1) {
		yyerror("srs already specified for this dispatcher");
		YYERROR;
	}

	dsp->u.remote.srs = 1;
}
break;
case 131:
#line 977 "../../usr.sbin/smtpd/parse.y"
{
	dsp->type = DISPATCHER_LOCAL;
}
break;
case 132:
#line 980 "../../usr.sbin/smtpd/parse.y"
{
	dsp->type = DISPATCHER_REMOTE;
}
break;
case 133:
#line 986 "../../usr.sbin/smtpd/parse.y"
{
	if (dsp->ttl) {
		yyerror("ttl already specified for this dispatcher");
		YYERROR;
	}

	dsp->ttl = delaytonum(yyvsp[0].v.string);
	if (dsp->ttl == -1) {
		yyerror("ttl delay \"%s\" is invalid", yyvsp[0].v.string);
		free(yyvsp[0].v.string);
		YYERROR;
	}
	free(yyvsp[0].v.string);
}
break;
case 136:
#line 1008 "../../usr.sbin/smtpd/parse.y"
{
	if (dict_get(conf->sc_dispatchers, yyvsp[0].v.string)) {
		yyerror("dispatcher already declared with that name: %s", yyvsp[0].v.string);
		YYERROR;
	}
	dsp = xcalloc(1, sizeof *dsp);
}
break;
case 137:
#line 1014 "../../usr.sbin/smtpd/parse.y"
{
	if (dsp->type == DISPATCHER_LOCAL)
		if (dsp->u.local.table_userbase == NULL)
			dsp->u.local.table_userbase = "<getpwnam>";
	dict_set(conf->sc_dispatchers, yyvsp[-3].v.string, dsp);
	dsp = NULL;
}
break;
case 138:
#line 1024 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = yyvsp[0].v.table;

	if (rule->flag_tag) {
		yyerror("tag already specified for this rule");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_LIST, K_STRING)) {
		yyerror("table \"%s\" may not be used for tag lookups",
		    t->t_name);
		YYERROR;
	}

	rule->flag_tag = yyvsp[-2].v.number ? -1 : 1;
	rule->table_tag = strdup(t->t_name);
}
break;
case 139:
#line 1042 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = yyvsp[0].v.table;

	if (rule->flag_tag) {
		yyerror("tag already specified for this rule");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_LIST, K_REGEX)) {
		yyerror("table \"%s\" may not be used for tag lookups",
		    t->t_name);
		YYERROR;
	}

	rule->flag_tag = yyvsp[-3].v.number ? -1 : 1;
	rule->flag_tag_regex = 1;
	rule->table_tag = strdup(t->t_name);
}
break;
case 140:
#line 1061 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = yyvsp[0].v.table;

	if (rule->flag_smtp_helo) {
		yyerror("helo already specified for this rule");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_LIST, K_DOMAIN)) {
		yyerror("table \"%s\" may not be used for helo lookups",
		    t->t_name);
		YYERROR;
	}

	rule->flag_smtp_helo = yyvsp[-2].v.number ? -1 : 1;
	rule->table_smtp_helo = strdup(t->t_name);
}
break;
case 141:
#line 1078 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = yyvsp[0].v.table;

	if (rule->flag_smtp_helo) {
		yyerror("helo already specified for this rule");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_LIST, K_REGEX)) {
		yyerror("table \"%s\" may not be used for helo lookups",
		    t->t_name);
		YYERROR;
	}

	rule->flag_smtp_helo = yyvsp[-3].v.number ? -1 : 1;
	rule->flag_smtp_helo_regex = 1;
	rule->table_smtp_helo = strdup(t->t_name);
}
break;
case 142:
#line 1096 "../../usr.sbin/smtpd/parse.y"
{
	if (rule->flag_smtp_starttls) {
		yyerror("tls already specified for this rule");
		YYERROR;
	}
	rule->flag_smtp_starttls = yyvsp[-1].v.number ? -1 : 1;
}
break;
case 143:
#line 1103 "../../usr.sbin/smtpd/parse.y"
{
	if (rule->flag_smtp_auth) {
		yyerror("auth already specified for this rule");
		YYERROR;
	}
	rule->flag_smtp_auth = yyvsp[-1].v.number ? -1 : 1;
}
break;
case 144:
#line 1110 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = yyvsp[0].v.table;

	if (rule->flag_smtp_auth) {
		yyerror("auth already specified for this rule");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_LIST|T_HASH, K_STRING|K_CREDENTIALS)) {
		yyerror("table \"%s\" may not be used for auth lookups",
		    t->t_name);
		YYERROR;
	}

	rule->flag_smtp_auth = yyvsp[-2].v.number ? -1 : 1;
	rule->table_smtp_auth = strdup(t->t_name);
}
break;
case 145:
#line 1127 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = yyvsp[0].v.table;

	if (rule->flag_smtp_auth) {
		yyerror("auth already specified for this rule");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_LIST, K_REGEX)) {
		yyerror("table \"%s\" may not be used for auth lookups",
		    t->t_name);
		YYERROR;
	}

	rule->flag_smtp_auth = yyvsp[-3].v.number ? -1 : 1;
	rule->flag_smtp_auth_regex = 1;
	rule->table_smtp_auth = strdup(t->t_name);
}
break;
case 146:
#line 1145 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = yyvsp[0].v.table;

	if (rule->flag_smtp_mail_from) {
		yyerror("mail-from already specified for this rule");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_LIST|T_HASH, K_MAILADDR)) {
		yyerror("table \"%s\" may not be used for mail-from lookups",
		    t->t_name);
		YYERROR;
	}

	rule->flag_smtp_mail_from = yyvsp[-2].v.number ? -1 : 1;
	rule->table_smtp_mail_from = strdup(t->t_name);
}
break;
case 147:
#line 1162 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = yyvsp[0].v.table;

	if (rule->flag_smtp_mail_from) {
		yyerror("mail-from already specified for this rule");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_LIST, K_REGEX)) {
		yyerror("table \"%s\" may not be used for mail-from lookups",
		    t->t_name);
		YYERROR;
	}

	rule->flag_smtp_mail_from = yyvsp[-3].v.number ? -1 : 1;
	rule->flag_smtp_mail_from_regex = 1;
	rule->table_smtp_mail_from = strdup(t->t_name);
}
break;
case 148:
#line 1180 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = yyvsp[0].v.table;

	if (rule->flag_smtp_rcpt_to) {
		yyerror("rcpt-to already specified for this rule");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_LIST|T_HASH, K_MAILADDR)) {
		yyerror("table \"%s\" may not be used for rcpt-to lookups",
		    t->t_name);
		YYERROR;
	}

	rule->flag_smtp_rcpt_to = yyvsp[-2].v.number ? -1 : 1;
	rule->table_smtp_rcpt_to = strdup(t->t_name);
}
break;
case 149:
#line 1197 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = yyvsp[0].v.table;

	if (rule->flag_smtp_rcpt_to) {
		yyerror("rcpt-to already specified for this rule");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_LIST, K_REGEX)) {
		yyerror("table \"%s\" may not be used for rcpt-to lookups",
		    t->t_name);
		YYERROR;
	}

	rule->flag_smtp_rcpt_to = yyvsp[-3].v.number ? -1 : 1;
	rule->flag_smtp_rcpt_to_regex = 1;
	rule->table_smtp_rcpt_to = strdup(t->t_name);
}
break;
case 150:
#line 1216 "../../usr.sbin/smtpd/parse.y"
{
	if (rule->flag_from) {
		yyerror("from already specified for this rule");
		YYERROR;
	}
	rule->flag_from = yyvsp[-2].v.number ? -1 : 1;
	rule->flag_from_socket = 1;
}
break;
case 151:
#line 1224 "../../usr.sbin/smtpd/parse.y"
{
	struct table	*t = table_find(conf, "<localhost>");

	if (rule->flag_from) {
		yyerror("from already specified for this rule");
		YYERROR;
	}
	rule->flag_from = yyvsp[-2].v.number ? -1 : 1;
	rule->table_from = strdup(t->t_name);
}
break;
case 152:
#line 1234 "../../usr.sbin/smtpd/parse.y"
{
	struct table	*t = table_find(conf, "<anyhost>");

	if (rule->flag_from) {
		yyerror("from already specified for this rule");
		YYERROR;
	}
	rule->flag_from = yyvsp[-2].v.number ? -1 : 1;
	rule->table_from = strdup(t->t_name);
}
break;
case 153:
#line 1244 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = yyvsp[0].v.table;

	if (rule->flag_from) {
		yyerror("from already specified for this rule");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_LIST, K_NETADDR)) {
		yyerror("table \"%s\" may not be used for from lookups",
		    t->t_name);
		YYERROR;
	}

	rule->flag_from = yyvsp[-3].v.number ? -1 : 1;
	rule->table_from = strdup(t->t_name);
}
break;
case 154:
#line 1261 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = yyvsp[0].v.table;

	if (rule->flag_from) {
		yyerror("from already specified for this rule");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_LIST, K_REGEX)) {
		yyerror("table \"%s\" may not be used for from lookups",
		    t->t_name);
		YYERROR;
	}

	rule->flag_from = yyvsp[-4].v.number ? -1 : 1;
	rule->flag_from_regex = 1;
	rule->table_from = strdup(t->t_name);
}
break;
case 155:
#line 1279 "../../usr.sbin/smtpd/parse.y"
{
	if (rule->flag_from) {
		yyerror("from already specified for this rule");
		YYERROR;
	}
	rule->flag_from = yyvsp[-2].v.number ? -1 : 1;
	rule->flag_from_rdns = 1;
}
break;
case 156:
#line 1287 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = yyvsp[0].v.table;

	if (rule->flag_from) {
		yyerror("from already specified for this rule");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_LIST, K_DOMAIN)) {
		yyerror("table \"%s\" may not be used for rdns lookups",
		    t->t_name);
		YYERROR;
	}

	rule->flag_from = yyvsp[-3].v.number ? -1 : 1;
	rule->flag_from_rdns = 1;
	rule->table_from = strdup(t->t_name);
}
break;
case 157:
#line 1305 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = yyvsp[0].v.table;

	if (rule->flag_from) {
		yyerror("from already specified for this rule");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_LIST, K_DOMAIN)) {
		yyerror("table \"%s\" may not be used for rdns lookups",
		    t->t_name);
		YYERROR;
	}

	rule->flag_from = yyvsp[-4].v.number ? -1 : 1;
	rule->flag_from_regex = 1;
	rule->flag_from_rdns = 1;
	rule->table_from = strdup(t->t_name);
}
break;
case 158:
#line 1325 "../../usr.sbin/smtpd/parse.y"
{
	struct table	*anyhost = table_find(conf, "<anyhost>");

	if (rule->flag_from) {
		yyerror("from already specified for this rule");
		YYERROR;
	}

	rule->flag_from = 1;
	rule->table_from = strdup(anyhost->t_name);
	rule->flag_smtp_auth = yyvsp[-2].v.number ? -1 : 1;
}
break;
case 159:
#line 1337 "../../usr.sbin/smtpd/parse.y"
{
	struct table	*anyhost = table_find(conf, "<anyhost>");
	struct table	*t = yyvsp[0].v.table;

	if (rule->flag_from) {
		yyerror("from already specified for this rule");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_LIST, K_STRING|K_CREDENTIALS)) {
		yyerror("table \"%s\" may not be used for from lookups",
		    t->t_name);
		YYERROR;
	}

	rule->flag_from = 1;
	rule->table_from = strdup(anyhost->t_name);
	rule->flag_smtp_auth = yyvsp[-3].v.number ? -1 : 1;
	rule->table_smtp_auth = strdup(t->t_name);
}
break;
case 160:
#line 1357 "../../usr.sbin/smtpd/parse.y"
{
	struct table	*anyhost = table_find(conf, "<anyhost>");
	struct table	*t = yyvsp[0].v.table;

	if (rule->flag_from) {
		yyerror("from already specified for this rule");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_LIST, K_REGEX)) {
        	yyerror("table \"%s\" may not be used for from lookups",
		    t->t_name);
		YYERROR;
	}

	rule->flag_from = 1;
	rule->table_from = strdup(anyhost->t_name);
	rule->flag_smtp_auth = yyvsp[-4].v.number ? -1 : 1;
	rule->flag_smtp_auth_regex = 1;
	rule->table_smtp_auth = strdup(t->t_name);
}
break;
case 161:
#line 1379 "../../usr.sbin/smtpd/parse.y"
{
	struct table	*anyhost = table_find(conf, "<anyhost>");
	struct table	*t = yyvsp[0].v.table;

	if (rule->flag_from) {
		yyerror("from already specified for this rule");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_LIST|T_HASH, K_MAILADDR)) {
		yyerror("table \"%s\" may not be used for from lookups",
		    t->t_name);
		YYERROR;
	}

	rule->flag_from = 1;
	rule->table_from = strdup(anyhost->t_name);
	rule->flag_smtp_mail_from = yyvsp[-3].v.number ? -1 : 1;
	rule->table_smtp_mail_from = strdup(t->t_name);
}
break;
case 162:
#line 1399 "../../usr.sbin/smtpd/parse.y"
{
	struct table	*anyhost = table_find(conf, "<anyhost>");
	struct table	*t = yyvsp[0].v.table;

	if (rule->flag_from) {
		yyerror("from already specified for this rule");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_LIST, K_REGEX)) {
		yyerror("table \"%s\" may not be used for from lookups",
		    t->t_name);
		YYERROR;
	}

	rule->flag_from = 1;
	rule->table_from = strdup(anyhost->t_name);
	rule->flag_smtp_mail_from = yyvsp[-4].v.number ? -1 : 1;
	rule->flag_smtp_mail_from_regex = 1;
	rule->table_smtp_mail_from = strdup(t->t_name);
}
break;
case 163:
#line 1421 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = table_find(conf, "<localnames>");

	if (rule->flag_for) {
		yyerror("for already specified for this rule");
		YYERROR;
	}
	rule->flag_for = yyvsp[-2].v.number ? -1 : 1;
	rule->table_for = strdup(t->t_name);
}
break;
case 164:
#line 1431 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = table_find(conf, "<anydestination>");

	if (rule->flag_for) {
		yyerror("for already specified for this rule");
		YYERROR;
	}
	rule->flag_for = yyvsp[-2].v.number ? -1 : 1;
	rule->table_for = strdup(t->t_name);
}
break;
case 165:
#line 1441 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = yyvsp[0].v.table;

	if (rule->flag_for) {
		yyerror("for already specified for this rule");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_LIST, K_DOMAIN)) {
		yyerror("table \"%s\" may not be used for 'for' lookups",
		    t->t_name);
		YYERROR;
	}

	rule->flag_for = yyvsp[-3].v.number ? -1 : 1;
	rule->table_for = strdup(t->t_name);
}
break;
case 166:
#line 1458 "../../usr.sbin/smtpd/parse.y"
{
	struct table   *t = yyvsp[0].v.table;

	if (rule->flag_for) {
		yyerror("for already specified for this rule");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_LIST, K_REGEX)) {
		yyerror("table \"%s\" may not be used for 'for' lookups",
		    t->t_name);
		YYERROR;
	}

	rule->flag_for = yyvsp[-4].v.number ? -1 : 1;
	rule->flag_for_regex = 1;
	rule->table_for = strdup(t->t_name);
}
break;
case 167:
#line 1476 "../../usr.sbin/smtpd/parse.y"
{
	struct table	*anyhost = table_find(conf, "<anydestination>");
	struct table	*t = yyvsp[0].v.table;

	if (rule->flag_for) {
		yyerror("for already specified for this rule");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_LIST|T_HASH, K_MAILADDR)) {
		yyerror("table \"%s\" may not be used for for lookups",
		    t->t_name);
		YYERROR;
	}

	rule->flag_for = 1;
	rule->table_for = strdup(anyhost->t_name);
	rule->flag_smtp_rcpt_to = yyvsp[-3].v.number ? -1 : 1;
	rule->table_smtp_rcpt_to = strdup(t->t_name);
}
break;
case 168:
#line 1496 "../../usr.sbin/smtpd/parse.y"
{
	struct table	*anyhost = table_find(conf, "<anydestination>");
	struct table	*t = yyvsp[0].v.table;

	if (rule->flag_for) {
		yyerror("for already specified for this rule");
		YYERROR;
	}

	if (!table_check_use(t, T_DYNAMIC|T_LIST, K_REGEX)) {
		yyerror("table \"%s\" may not be used for for lookups",
		    t->t_name);
		YYERROR;
	}

	rule->flag_for = 1;
	rule->table_for = strdup(anyhost->t_name);
	rule->flag_smtp_rcpt_to = yyvsp[-4].v.number ? -1 : 1;
	rule->flag_smtp_rcpt_to_regex = 1;
	rule->table_smtp_rcpt_to = strdup(t->t_name);
}
break;
case 171:
#line 1525 "../../usr.sbin/smtpd/parse.y"
{
	if (dict_get(conf->sc_dispatchers, yyvsp[0].v.string) == NULL) {
		yyerror("no such dispatcher: %s", yyvsp[0].v.string);
		YYERROR;
	}
	rule->dispatcher = yyvsp[0].v.string;
}
break;
case 172:
#line 1535 "../../usr.sbin/smtpd/parse.y"
{
	rule->reject = 1;
}
break;
case 174:
#line 1542 "../../usr.sbin/smtpd/parse.y"
{
	rule = xcalloc(1, sizeof *rule);
}
break;
case 175:
#line 1544 "../../usr.sbin/smtpd/parse.y"
{
	if (!rule->flag_from) {
		rule->table_from = strdup("<localhost>");
		rule->flag_from = 1;
	}
	if (!rule->flag_for) {
		rule->table_for = strdup("<localnames>");
		rule->flag_for = 1;
	}
	TAILQ_INSERT_TAIL(conf->sc_rules, rule, r_entry);
	rule = NULL;
}
break;
case 177:
#line 1560 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->junk = 1;
}
break;
case 178:
#line 1563 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->bypass = 1;
}
break;
case 179:
#line 1569 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->reject = yyvsp[0].v.string;
}
break;
case 180:
#line 1572 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->disconnect = yyvsp[0].v.string;
}
break;
case 181:
#line 1575 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->rewrite = yyvsp[0].v.string;
}
break;
case 182:
#line 1578 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->report = yyvsp[0].v.string;
}
break;
case 183:
#line 1584 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->not_fcrdns = yyvsp[-1].v.number ? -1 : 1;
	filter_config->fcrdns = 1;
}
break;
case 184:
#line 1591 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->not_rdns = yyvsp[-1].v.number ? -1 : 1;
	filter_config->rdns = 1;
}
break;
case 185:
#line 1598 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->not_rdns_table = yyvsp[-2].v.number ? -1 : 1;
	filter_config->rdns_table = yyvsp[0].v.table;
}
break;
case 186:
#line 1604 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->not_rdns_regex = yyvsp[-3].v.number ? -1 : 1;
	filter_config->rdns_regex = yyvsp[0].v.table;
}
break;
case 187:
#line 1611 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->not_src_table = yyvsp[-2].v.number ? -1 : 1;
	filter_config->src_table = yyvsp[0].v.table;
}
break;
case 188:
#line 1617 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->not_src_regex = yyvsp[-3].v.number ? -1 : 1;
	filter_config->src_regex = yyvsp[0].v.table;
}
break;
case 189:
#line 1624 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->not_helo_table = yyvsp[-2].v.number ? -1 : 1;
	filter_config->helo_table = yyvsp[0].v.table;
}
break;
case 190:
#line 1630 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->not_helo_regex = yyvsp[-3].v.number ? -1 : 1;
	filter_config->helo_regex = yyvsp[0].v.table;
}
break;
case 191:
#line 1637 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->not_auth = yyvsp[-1].v.number ? -1 : 1;
	filter_config->auth = 1;
}
break;
case 192:
#line 1643 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->not_auth_table = yyvsp[-2].v.number ? -1 : 1;
	filter_config->auth_table = yyvsp[0].v.table;
}
break;
case 193:
#line 1649 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->not_auth_regex = yyvsp[-3].v.number ? -1 : 1;
	filter_config->auth_regex = yyvsp[0].v.table;
}
break;
case 194:
#line 1656 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->not_mail_from_table = yyvsp[-2].v.number ? -1 : 1;
	filter_config->mail_from_table = yyvsp[0].v.table;
}
break;
case 195:
#line 1662 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->not_mail_from_regex = yyvsp[-3].v.number ? -1 : 1;
	filter_config->mail_from_regex = yyvsp[0].v.table;
}
break;
case 196:
#line 1669 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->not_rcpt_to_table = yyvsp[-2].v.number ? -1 : 1;
	filter_config->rcpt_to_table = yyvsp[0].v.table;
}
break;
case 197:
#line 1675 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->not_rcpt_to_regex = yyvsp[-3].v.number ? -1 : 1;
	filter_config->rcpt_to_regex = yyvsp[0].v.table;
}
break;
case 248:
#line 1766 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->phase = FILTER_CONNECT;
}
break;
case 250:
#line 1773 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->phase = FILTER_HELO;
}
break;
case 252:
#line 1779 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->phase = FILTER_EHLO;
}
break;
case 254:
#line 1785 "../../usr.sbin/smtpd/parse.y"
{
}
break;
case 256:
#line 1790 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->phase = FILTER_MAIL_FROM;
}
break;
case 258:
#line 1796 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->phase = FILTER_RCPT_TO;
}
break;
case 260:
#line 1802 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->phase = FILTER_DATA;
}
break;
case 262:
#line 1834 "../../usr.sbin/smtpd/parse.y"
{
	filter_config->phase = FILTER_COMMIT;
}
break;
case 272:
#line 1858 "../../usr.sbin/smtpd/parse.y"
{
	struct filter_config   *fr;
	size_t			i;

	if ((fr = dict_get(conf->sc_filters_dict, yyvsp[0].v.string)) == NULL) {
		yyerror("no filter exist with that name: %s", yyvsp[0].v.string);
		free(yyvsp[0].v.string);
		YYERROR;
	}
	if (fr->filter_type == FILTER_TYPE_CHAIN) {
		yyerror("no filter chain allowed within a filter chain: %s", yyvsp[0].v.string);
		free(yyvsp[0].v.string);
		YYERROR;
	}

	for (i = 0; i < filter_config->chain_size; i++) {
		if (strcmp(filter_config->chain[i], yyvsp[0].v.string) == 0) {
			yyerror("no filter allowed twice within a filter chain: %s", yyvsp[0].v.string);
			free(yyvsp[0].v.string);
			YYERROR;
		}
	}

	if (fr->proc) {
		if (dict_get(&filter_config->chain_procs, fr->proc)) {
			yyerror("no proc allowed twice within a filter chain: %s", fr->proc);
			free(yyvsp[0].v.string);
			YYERROR;
		}
		dict_set(&filter_config->chain_procs, fr->proc, NULL);
	}

	fr->filter_subsystem |= filter_config->filter_subsystem;
	filter_config->chain_size += 1;
	filter_config->chain = reallocarray(filter_config->chain, filter_config->chain_size, sizeof(char *));
	if (filter_config->chain == NULL)
		fatal("reallocarray");
	filter_config->chain[filter_config->chain_size - 1] = yyvsp[0].v.string;
}
break;
case 275:
#line 1905 "../../usr.sbin/smtpd/parse.y"
{

	if (dict_get(conf->sc_filters_dict, yyvsp[-2].v.string)) {
		yyerror("filter already exists with that name: %s", yyvsp[-2].v.string);
		free(yyvsp[-2].v.string);
		free(yyvsp[0].v.string);
		YYERROR;
	}
	if (dict_get(conf->sc_filter_processes_dict, yyvsp[0].v.string) == NULL) {
		yyerror("no processor exist with that name: %s", yyvsp[0].v.string);
		free(yyvsp[0].v.string);
		YYERROR;
	}

	filter_config = xcalloc(1, sizeof *filter_config);
	filter_config->filter_type = FILTER_TYPE_PROC;
	filter_config->name = yyvsp[-2].v.string;
	filter_config->proc = yyvsp[0].v.string;
	dict_set(conf->sc_filters_dict, yyvsp[-2].v.string, filter_config);
	filter_config = NULL;
}
break;
case 276:
#line 1927 "../../usr.sbin/smtpd/parse.y"
{
	if (dict_get(conf->sc_filters_dict, yyvsp[-2].v.string)) {
		yyerror("filter already exists with that name: %s", yyvsp[-2].v.string);
		free(yyvsp[-2].v.string);
		free(yyvsp[0].v.string);
		YYERROR;
	}

	processor = xcalloc(1, sizeof *processor);
	processor->command = yyvsp[0].v.string;

	filter_config = xcalloc(1, sizeof *filter_config);
	filter_config->filter_type = FILTER_TYPE_PROC;
	filter_config->name = yyvsp[-2].v.string;
	filter_config->proc = xstrdup(yyvsp[-2].v.string);
	dict_set(conf->sc_filters_dict, yyvsp[-2].v.string, filter_config);
}
break;
case 277:
#line 1943 "../../usr.sbin/smtpd/parse.y"
{
	dict_set(conf->sc_filter_processes_dict, filter_config->proc, processor);
	processor = NULL;
	filter_config = NULL;
}
break;
case 278:
#line 1949 "../../usr.sbin/smtpd/parse.y"
{
	if (dict_get(conf->sc_filters_dict, yyvsp[-1].v.string)) {
		yyerror("filter already exists with that name: %s", yyvsp[-1].v.string);
		free(yyvsp[-1].v.string);
		YYERROR;
	}
	filter_config = xcalloc(1, sizeof *filter_config);
	filter_config->name = yyvsp[-1].v.string;
	filter_config->filter_type = FILTER_TYPE_BUILTIN;
	dict_set(conf->sc_filters_dict, yyvsp[-1].v.string, filter_config);
}
break;
case 279:
#line 1959 "../../usr.sbin/smtpd/parse.y"
{
	filter_config = NULL;
}
break;
case 280:
#line 1963 "../../usr.sbin/smtpd/parse.y"
{
	if (dict_get(conf->sc_filters_dict, yyvsp[-1].v.string)) {
		yyerror("filter already exists with that name: %s", yyvsp[-1].v.string);
		free(yyvsp[-1].v.string);
		YYERROR;
	}
	filter_config = xcalloc(1, sizeof *filter_config);
	filter_config->filter_type = FILTER_TYPE_CHAIN;
	dict_init(&filter_config->chain_procs);
}
break;
case 281:
#line 1972 "../../usr.sbin/smtpd/parse.y"
{
	dict_set(conf->sc_filters_dict, yyvsp[-6].v.string, filter_config);
	filter_config = NULL;
}
break;
case 282:
#line 1978 "../../usr.sbin/smtpd/parse.y"
{
			if (yyvsp[0].v.number < 0) {
				yyerror("invalid size: %" PRId64, yyvsp[0].v.number);
				YYERROR;
			}
			yyval.v.number = yyvsp[0].v.number;
		}
break;
case 283:
#line 1985 "../../usr.sbin/smtpd/parse.y"
{
			long long result;

			if (scan_scaled(yyvsp[0].v.string, &result) == -1 || result < 0) {
				yyerror("invalid size: %s", yyvsp[0].v.string);
				free(yyvsp[0].v.string);
				YYERROR;
			}
			free(yyvsp[0].v.string);
			yyval.v.number = result;
		}
break;
case 284:
#line 1998 "../../usr.sbin/smtpd/parse.y"
{
			time_t	d;
			int	i;

			d = delaytonum(yyvsp[0].v.string);
			if (d < 0) {
				yyerror("invalid bounce delay: %s", yyvsp[0].v.string);
				free(yyvsp[0].v.string);
				YYERROR;
			}
			free(yyvsp[0].v.string);
			for (i = 0; i < MAX_BOUNCE_WARN; i++) {
				if (conf->sc_bounce_warn[i] != 0)
					continue;
				conf->sc_bounce_warn[i] = d;
				break;
			}
		}
break;
case 287:
#line 2022 "../../usr.sbin/smtpd/parse.y"
{
			if (!strcmp(yyvsp[-1].v.string, "max-session")) {
				conf->sc_mda_max_session = yyvsp[0].v.number;
			}
			else if (!strcmp(yyvsp[-1].v.string, "max-session-per-user")) {
				conf->sc_mda_max_user_session = yyvsp[0].v.number;
			}
			else if (!strcmp(yyvsp[-1].v.string, "task-lowat")) {
				conf->sc_mda_task_lowat = yyvsp[0].v.number;
			}
			else if (!strcmp(yyvsp[-1].v.string, "task-hiwat")) {
				conf->sc_mda_task_hiwat = yyvsp[0].v.number;
			}
			else if (!strcmp(yyvsp[-1].v.string, "task-release")) {
				conf->sc_mda_task_release = yyvsp[0].v.number;
			}
			else {
				yyerror("invalid scheduler limit keyword: %s", yyvsp[-1].v.string);
				free(yyvsp[-1].v.string);
				YYERROR;
			}
			free(yyvsp[-1].v.string);
		}
break;
case 290:
#line 2051 "../../usr.sbin/smtpd/parse.y"
{
			if (!strcmp(yyvsp[-1].v.string, "max-rcpt")) {
				conf->sc_session_max_rcpt = yyvsp[0].v.number;
			}
			else if (!strcmp(yyvsp[-1].v.string, "max-mails")) {
				conf->sc_session_max_mails = yyvsp[0].v.number;
			}
			else {
				yyerror("invalid session limit keyword: %s", yyvsp[-1].v.string);
				free(yyvsp[-1].v.string);
				YYERROR;
			}
			free(yyvsp[-1].v.string);
		}
break;
case 293:
#line 2071 "../../usr.sbin/smtpd/parse.y"
{
			limits->family = AF_INET;
		}
break;
case 294:
#line 2074 "../../usr.sbin/smtpd/parse.y"
{
			limits->family = AF_INET6;
		}
break;
case 295:
#line 2077 "../../usr.sbin/smtpd/parse.y"
{
			if (!limit_mta_set(limits, yyvsp[-1].v.string, yyvsp[0].v.number)) {
				yyerror("invalid mta limit keyword: %s", yyvsp[-1].v.string);
				free(yyvsp[-1].v.string);
				YYERROR;
			}
			free(yyvsp[-1].v.string);
		}
break;
case 298:
#line 2091 "../../usr.sbin/smtpd/parse.y"
{
			if (!strcmp(yyvsp[-1].v.string, "max-inflight")) {
				conf->sc_scheduler_max_inflight = yyvsp[0].v.number;
			}
			else if (!strcmp(yyvsp[-1].v.string, "max-evp-batch-size")) {
				conf->sc_scheduler_max_evp_batch_size = yyvsp[0].v.number;
			}
			else if (!strcmp(yyvsp[-1].v.string, "max-msg-batch-size")) {
				conf->sc_scheduler_max_msg_batch_size = yyvsp[0].v.number;
			}
			else if (!strcmp(yyvsp[-1].v.string, "max-schedule")) {
				conf->sc_scheduler_max_schedule = yyvsp[0].v.number;
			}
			else {
				yyerror("invalid scheduler limit keyword: %s", yyvsp[-1].v.string);
				free(yyvsp[-1].v.string);
				YYERROR;
			}
			free(yyvsp[-1].v.string);
		}
break;
case 301:
#line 2118 "../../usr.sbin/smtpd/parse.y"
{
			struct filter_config *fc;

			if (listen_opts.options & LO_FILTER) {
				yyerror("filter already specified");
				free(yyvsp[0].v.string);
				YYERROR;
			}
			if ((fc = dict_get(conf->sc_filters_dict, yyvsp[0].v.string)) == NULL) {
				yyerror("no filter exist with that name: %s", yyvsp[0].v.string);
				free(yyvsp[0].v.string);
				YYERROR;
			}
			fc->filter_subsystem |= FILTER_SUBSYSTEM_SMTP_IN;
			listen_opts.options |= LO_FILTER;
			listen_opts.filtername = yyvsp[0].v.string;
		}
break;
case 302:
#line 2135 "../../usr.sbin/smtpd/parse.y"
{
			char	buffer[128];

			if (listen_opts.options & LO_FILTER) {
				yyerror("filter already specified");
				YYERROR;
			}

			do {
				(void)snprintf(buffer, sizeof buffer, "<dynchain:%08x>", last_dynchain_id++);
			} while (dict_check(conf->sc_filters_dict, buffer));

			listen_opts.options |= LO_FILTER;
			listen_opts.filtername = xstrdup(buffer);
			filter_config = xcalloc(1, sizeof *filter_config);
			filter_config->filter_type = FILTER_TYPE_CHAIN;
			filter_config->filter_subsystem |= FILTER_SUBSYSTEM_SMTP_IN;
			dict_init(&filter_config->chain_procs);
		}
break;
case 303:
#line 2153 "../../usr.sbin/smtpd/parse.y"
{
			dict_set(conf->sc_filters_dict, listen_opts.filtername, filter_config);
			filter_config = NULL;
		}
break;
case 304:
#line 2157 "../../usr.sbin/smtpd/parse.y"
{
			if (config_lo_mask_source(&listen_opts)) {
				YYERROR;
			}
		}
break;
case 305:
#line 2162 "../../usr.sbin/smtpd/parse.y"
{
			if (listen_opts.options & LO_NODSN) {
				yyerror("no-dsn already specified");
				YYERROR;
			}
			listen_opts.options |= LO_NODSN;
			listen_opts.flags &= ~F_EXT_DSN;
		}
break;
case 306:
#line 2170 "../../usr.sbin/smtpd/parse.y"
{
			if (listen_opts.options & LO_TAG) {
				yyerror("tag already specified");
				YYERROR;
			}
			listen_opts.options |= LO_TAG;

			if (strlen(yyvsp[0].v.string) >= SMTPD_TAG_SIZE) {
				yyerror("tag name too long");
				free(yyvsp[0].v.string);
				YYERROR;
			}
			listen_opts.tag = yyvsp[0].v.string;
		}
break;
case 307:
#line 2186 "../../usr.sbin/smtpd/parse.y"
{
			if (listen_opts.options & LO_FAMILY) {
				yyerror("address family already specified");
				YYERROR;
			}
			listen_opts.options |= LO_FAMILY;
			listen_opts.family = AF_INET;
		}
break;
case 308:
#line 2194 "../../usr.sbin/smtpd/parse.y"
{
			if (listen_opts.options & LO_FAMILY) {
				yyerror("address family already specified");
				YYERROR;
			}
			listen_opts.options |= LO_FAMILY;
			listen_opts.family = AF_INET6;
		}
break;
case 309:
#line 2202 "../../usr.sbin/smtpd/parse.y"
{
			struct servent	*servent;

			if (listen_opts.options & LO_PORT) {
				yyerror("port already specified");
				YYERROR;
			}
			listen_opts.options |= LO_PORT;

			servent = getservbyname(yyvsp[0].v.string, "tcp");
			if (servent == NULL) {
				yyerror("invalid port: %s", yyvsp[0].v.string);
				free(yyvsp[0].v.string);
				YYERROR;
			}
			free(yyvsp[0].v.string);
			listen_opts.port = ntohs(servent->s_port);
		}
break;
case 310:
#line 2220 "../../usr.sbin/smtpd/parse.y"
{
			struct servent *servent;

			if (listen_opts.options & LO_PORT) {
				yyerror("port already specified");
				YYERROR;
			}
			listen_opts.options |= LO_PORT;

			servent = getservbyname("smtp", "tcp");
			if (servent == NULL) {
				yyerror("invalid port: smtp");
				YYERROR;
			}
			listen_opts.port = ntohs(servent->s_port);
		}
break;
case 311:
#line 2236 "../../usr.sbin/smtpd/parse.y"
{
			struct servent *servent;

			if (listen_opts.options & LO_PORT) {
				yyerror("port already specified");
				YYERROR;
			}
			listen_opts.options |= LO_PORT;

			servent = getservbyname("smtps", "tcp");
			if (servent == NULL) {
				yyerror("invalid port: smtps");
				YYERROR;
			}
			listen_opts.port = ntohs(servent->s_port);
		}
break;
case 312:
#line 2252 "../../usr.sbin/smtpd/parse.y"
{
			if (listen_opts.options & LO_PORT) {
				yyerror("port already specified");
				YYERROR;
			}
			listen_opts.options |= LO_PORT;

			if (yyvsp[0].v.number <= 0 || yyvsp[0].v.number > (int)USHRT_MAX) {
				yyerror("invalid port: %" PRId64, yyvsp[0].v.number);
				YYERROR;
			}
			listen_opts.port = yyvsp[0].v.number;
		}
break;
case 313:
#line 2265 "../../usr.sbin/smtpd/parse.y"
{
			struct filter_config *fc;

			if (listen_opts.options & LO_FILTER) {
				yyerror("filter already specified");
				YYERROR;
			}
			if ((fc = dict_get(conf->sc_filters_dict, yyvsp[0].v.string)) == NULL) {
				yyerror("no filter exist with that name: %s", yyvsp[0].v.string);
				free(yyvsp[0].v.string);
				YYERROR;
			}
			fc->filter_subsystem |= FILTER_SUBSYSTEM_SMTP_IN;
			listen_opts.options |= LO_FILTER;
			listen_opts.filtername = yyvsp[0].v.string;
		}
break;
case 314:
#line 2281 "../../usr.sbin/smtpd/parse.y"
{
			char	buffer[128];

			if (listen_opts.options & LO_FILTER) {
				yyerror("filter already specified");
				YYERROR;
			}

			do {
				(void)snprintf(buffer, sizeof buffer, "<dynchain:%08x>", last_dynchain_id++);
			} while (dict_check(conf->sc_filters_dict, buffer));

			listen_opts.options |= LO_FILTER;
			listen_opts.filtername = xstrdup(buffer);
			filter_config = xcalloc(1, sizeof *filter_config);
			filter_config->filter_type = FILTER_TYPE_CHAIN;
			filter_config->filter_subsystem |= FILTER_SUBSYSTEM_SMTP_IN;
			dict_init(&filter_config->chain_procs);
		}
break;
case 315:
#line 2299 "../../usr.sbin/smtpd/parse.y"
{
			dict_set(conf->sc_filters_dict, listen_opts.filtername, filter_config);
			filter_config = NULL;
		}
break;
case 316:
#line 2303 "../../usr.sbin/smtpd/parse.y"
{
			if (listen_opts.options & LO_SSL) {
				yyerror("TLS mode already specified");
				YYERROR;
			}
			listen_opts.options |= LO_SSL;
			listen_opts.ssl = F_SMTPS;
		}
break;
case 317:
#line 2311 "../../usr.sbin/smtpd/parse.y"
{
			if (listen_opts.options & LO_SSL) {
				yyerror("TLS mode already specified");
				YYERROR;
			}
			listen_opts.options |= LO_SSL;
			listen_opts.ssl = F_SMTPS|F_TLS_VERIFY;
		}
break;
case 318:
#line 2319 "../../usr.sbin/smtpd/parse.y"
{
			if (listen_opts.options & LO_SSL) {
				yyerror("TLS mode already specified");
				YYERROR;
			}
			listen_opts.options |= LO_SSL;
			listen_opts.ssl = F_STARTTLS;
		}
break;
case 319:
#line 2327 "../../usr.sbin/smtpd/parse.y"
{
			if (listen_opts.options & LO_SSL) {
				yyerror("TLS mode already specified");
				YYERROR;
			}
			listen_opts.options |= LO_SSL;
			listen_opts.ssl = F_STARTTLS|F_STARTTLS_REQUIRE;
		}
break;
case 320:
#line 2335 "../../usr.sbin/smtpd/parse.y"
{
			if (listen_opts.options & LO_SSL) {
				yyerror("TLS mode already specified");
				YYERROR;
			}
			listen_opts.options |= LO_SSL;
			listen_opts.ssl = F_STARTTLS|F_STARTTLS_REQUIRE|F_TLS_VERIFY;
		}
break;
case 321:
#line 2343 "../../usr.sbin/smtpd/parse.y"
{
			if (listen_opts.tls_ciphers) {
				yyerror("ciphers already specified");
				YYERROR;
			}
			listen_opts.tls_ciphers = yyvsp[0].v.string;
		}
break;
case 322:
#line 2350 "../../usr.sbin/smtpd/parse.y"
{
			if (listen_opts.tls_protocols) {
				yyerror("protocols already specified");
				YYERROR;
			}
			listen_opts.tls_protocols = yyvsp[0].v.string;
		}
break;
case 323:
#line 2357 "../../usr.sbin/smtpd/parse.y"
{
			if (listen_opts.pkicount == PKI_MAX) {
				yyerror("too many pki specified");
				YYERROR;
			}
			listen_opts.pki[listen_opts.pkicount++] = yyvsp[0].v.string;
		}
break;
case 324:
#line 2364 "../../usr.sbin/smtpd/parse.y"
{
			if (listen_opts.options & LO_CA) {
				yyerror("ca already specified");
				YYERROR;
			}
			listen_opts.options |= LO_CA;
			listen_opts.ca = yyvsp[0].v.string;
		}
break;
case 325:
#line 2372 "../../usr.sbin/smtpd/parse.y"
{
			if (listen_opts.options & LO_AUTH) {
				yyerror("auth already specified");
				YYERROR;
			}
			listen_opts.options |= LO_AUTH;
			listen_opts.auth = F_AUTH|F_AUTH_REQUIRE;
		}
break;
case 326:
#line 2380 "../../usr.sbin/smtpd/parse.y"
{
			if (listen_opts.options & LO_AUTH) {
				yyerror("auth already specified");
				YYERROR;
			}
			listen_opts.options |= LO_AUTH;
			listen_opts.auth = F_AUTH;
		}
break;
case 327:
#line 2388 "../../usr.sbin/smtpd/parse.y"
{
			if (listen_opts.options & LO_AUTH) {
				yyerror("auth already specified");
				YYERROR;
			}
			listen_opts.options |= LO_AUTH;
			listen_opts.authtable = yyvsp[0].v.table;
			listen_opts.auth = F_AUTH|F_AUTH_REQUIRE;
		}
break;
case 328:
#line 2397 "../../usr.sbin/smtpd/parse.y"
{
			if (listen_opts.options & LO_AUTH) {
				yyerror("auth already specified");
				YYERROR;
			}
			listen_opts.options |= LO_AUTH;
			listen_opts.authtable = yyvsp[0].v.table;
			listen_opts.auth = F_AUTH;
		}
break;
case 329:
#line 2406 "../../usr.sbin/smtpd/parse.y"
{
			if (listen_opts.options & LO_TAG) {
				yyerror("tag already specified");
				YYERROR;
			}
			listen_opts.options |= LO_TAG;

			if (strlen(yyvsp[0].v.string) >= SMTPD_TAG_SIZE) {
       				yyerror("tag name too long");
				free(yyvsp[0].v.string);
				YYERROR;
			}
			listen_opts.tag = yyvsp[0].v.string;
		}
break;
case 330:
#line 2420 "../../usr.sbin/smtpd/parse.y"
{
			if (listen_opts.options & LO_HOSTNAME) {
				yyerror("hostname already specified");
				YYERROR;
			}
			listen_opts.options |= LO_HOSTNAME;

			listen_opts.hostname = yyvsp[0].v.string;
		}
break;
case 331:
#line 2429 "../../usr.sbin/smtpd/parse.y"
{
			struct table	*t = yyvsp[0].v.table;

			if (listen_opts.options & LO_HOSTNAMES) {
				yyerror("hostnames already specified");
				YYERROR;
			}
			listen_opts.options |= LO_HOSTNAMES;

			if (!table_check_use(t, T_DYNAMIC|T_HASH, K_ADDRNAME)) {
				yyerror("invalid use of table \"%s\" as "
				    "HOSTNAMES parameter", t->t_name);
				YYERROR;
			}
			listen_opts.hostnametable = t;
		}
break;
case 332:
#line 2445 "../../usr.sbin/smtpd/parse.y"
{
			if (config_lo_mask_source(&listen_opts)) {
				YYERROR;
			}
		}
break;
case 333:
#line 2450 "../../usr.sbin/smtpd/parse.y"
{
			if (listen_opts.options & LO_RECEIVEDAUTH) {
				yyerror("received-auth already specified");
				YYERROR;
			}
			listen_opts.options |= LO_RECEIVEDAUTH;
			listen_opts.flags |= F_RECEIVEDAUTH;
		}
break;
case 334:
#line 2458 "../../usr.sbin/smtpd/parse.y"
{
			if (listen_opts.options & LO_NODSN) {
				yyerror("no-dsn already specified");
				YYERROR;
			}
			listen_opts.options |= LO_NODSN;
			listen_opts.flags &= ~F_EXT_DSN;
		}
break;
case 335:
#line 2466 "../../usr.sbin/smtpd/parse.y"
{
			if (listen_opts.options & LO_PROXY) {
				yyerror("proxy-v2 already specified");
				YYERROR;
			}
			listen_opts.options |= LO_PROXY;
			listen_opts.flags |= F_PROXY;
		}
break;
case 336:
#line 2474 "../../usr.sbin/smtpd/parse.y"
{
			struct table	*t = yyvsp[0].v.table;

			if (listen_opts.options & LO_SENDERS) {
				yyerror("senders already specified");
				YYERROR;
			}
			listen_opts.options |= LO_SENDERS;

			if (!table_check_use(t, T_DYNAMIC|T_HASH, K_MAILADDRMAP)) {
				yyerror("invalid use of table \"%s\" as "
				    "SENDERS parameter", t->t_name);
				YYERROR;
			}
			listen_opts.sendertable = t;
		}
break;
case 337:
#line 2490 "../../usr.sbin/smtpd/parse.y"
{
			struct table	*t = yyvsp[-1].v.table;

			if (listen_opts.options & LO_SENDERS) {
				yyerror("senders already specified");
				YYERROR;
			}
			listen_opts.options |= LO_SENDERS|LO_MASQUERADE;

			if (!table_check_use(t, T_DYNAMIC|T_HASH, K_MAILADDRMAP)) {
				yyerror("invalid use of table \"%s\" as "
				    "SENDERS parameter", t->t_name);
				YYERROR;
			}
			listen_opts.sendertable = t;
		}
break;
case 340:
#line 2512 "../../usr.sbin/smtpd/parse.y"
{
			if (conf->sc_sock_listener) {
				yyerror("socket listener already configured");
				YYERROR;
			}
			create_sock_listener(&listen_opts);
		}
break;
case 341:
#line 2521 "../../usr.sbin/smtpd/parse.y"
{
			listen_opts.ifx = yyvsp[-1].v.string;
			create_if_listener(&listen_opts);
		}
break;
case 346:
#line 2536 "../../usr.sbin/smtpd/parse.y"
{
			memset(&listen_opts, 0, sizeof listen_opts);
			listen_opts.family = AF_UNSPEC;
			listen_opts.flags |= F_EXT_DSN;
		}
break;
case 347:
#line 2540 "../../usr.sbin/smtpd/parse.y"
{
			free(listen_opts.tls_protocols);
			free(listen_opts.tls_ciphers);
			memset(&listen_opts, 0, sizeof listen_opts);
		}
break;
case 348:
#line 2547 "../../usr.sbin/smtpd/parse.y"
{
			char *p, *backend, *config;

			p = yyvsp[0].v.string;
			if (*p == '/') {
				backend = "static";
				config = yyvsp[0].v.string;
			}
			else {
				backend = yyvsp[0].v.string;
				config = NULL;
				for (p = yyvsp[0].v.string; *p && *p != ':'; p++)
					;
				if (*p == ':') {
					*p = '\0';
					backend = yyvsp[0].v.string;
					config  = p+1;
				}
			}
			if (config != NULL && *config != '/') {
				yyerror("invalid backend parameter for table: %s",
				    yyvsp[-1].v.string);
				free(yyvsp[-1].v.string);
				free(yyvsp[0].v.string);
				YYERROR;
			}
			table = table_create(conf, backend, yyvsp[-1].v.string, config);
			if (!table_config(table)) {
				yyerror("invalid configuration file %s for table %s",
				    config, table->t_name);
				free(yyvsp[-1].v.string);
				free(yyvsp[0].v.string);
				YYERROR;
			}
			table = NULL;
			free(yyvsp[-1].v.string);
			free(yyvsp[0].v.string);
		}
break;
case 349:
#line 2585 "../../usr.sbin/smtpd/parse.y"
{
			table = table_create(conf, "static", yyvsp[0].v.string, NULL);
			free(yyvsp[0].v.string);
		}
break;
case 350:
#line 2588 "../../usr.sbin/smtpd/parse.y"
{
			table = NULL;
		}
break;
case 351:
#line 2593 "../../usr.sbin/smtpd/parse.y"
{
			struct table	*t;

			t = table_create(conf, "static", NULL, NULL);
			table_add(t, yyvsp[0].v.string, NULL);
			free(yyvsp[0].v.string);
			yyval.v.table = t;
		}
break;
case 352:
#line 2601 "../../usr.sbin/smtpd/parse.y"
{
			table = table_create(conf, "static", NULL, NULL);
		}
break;
case 353:
#line 2603 "../../usr.sbin/smtpd/parse.y"
{
			yyval.v.table = table;
			table = NULL;
		}
break;
case 354:
#line 2609 "../../usr.sbin/smtpd/parse.y"
{
			struct table	*t;

			if ((t = table_find(conf, yyvsp[-1].v.string)) == NULL) {
				yyerror("no such table: %s", yyvsp[-1].v.string);
				free(yyvsp[-1].v.string);
				YYERROR;
			}
			free(yyvsp[-1].v.string);
			yyval.v.table = t;
		}
break;
case 355:
#line 2622 "../../usr.sbin/smtpd/parse.y"
{ yyval.v.table = yyvsp[0].v.table; }
break;
case 356:
#line 2623 "../../usr.sbin/smtpd/parse.y"
{ yyval.v.table = yyvsp[0].v.table; }
break;
#line 4986 "../../usr.sbin/smtpd/parse.c"
    }
    yyssp -= yym;
    yystate = *yyssp;
    yyvsp -= yym;
    yym = yylhs[yyn];
    if (yystate == 0 && yym == 0)
    {
#if YYDEBUG
        if (yydebug)
            printf("%sdebug: after reduction, shifting from state 0 to\
 state %d\n", YYPREFIX, YYFINAL);
#endif
        yystate = YYFINAL;
        *++yyssp = YYFINAL;
        *++yyvsp = yyval;
        if (yychar < 0)
        {
            if ((yychar = yylex()) < 0) yychar = 0;
#if YYDEBUG
            if (yydebug)
            {
                yys = 0;
                if (yychar <= YYMAXTOKEN) yys = yyname[yychar];
                if (!yys) yys = "illegal-symbol";
                printf("%sdebug: state %d, reading %d (%s)\n",
                        YYPREFIX, YYFINAL, yychar, yys);
            }
#endif
        }
        if (yychar == 0) goto yyaccept;
        goto yyloop;
    }
    if ((yyn = yygindex[yym]) && (yyn += yystate) >= 0 &&
            yyn <= YYTABLESIZE && yycheck[yyn] == yystate)
        yystate = yytable[yyn];
    else
        yystate = yydgoto[yym];
#if YYDEBUG
    if (yydebug)
        printf("%sdebug: after reduction, shifting from state %d \
to state %d\n", YYPREFIX, *yyssp, yystate);
#endif
    if (yyssp >= yysslim && yygrowstack())
    {
        goto yyoverflow;
    }
    *++yyssp = yystate;
    *++yyvsp = yyval;
    goto yyloop;
yyoverflow:
    yyerror("yacc stack overflow");
yyabort:
    if (yyss)
            free(yyss);
    if (yyvs)
            free(yyvs);
    yyss = yyssp = NULL;
    yyvs = yyvsp = NULL;
    yystacksize = 0;
    return (1);
yyaccept:
    if (yyss)
            free(yyss);
    if (yyvs)
            free(yyvs);
    yyss = yyssp = NULL;
    yyvs = yyvsp = NULL;
    yystacksize = 0;
    return (0);
}
