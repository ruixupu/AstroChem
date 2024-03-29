/*==============================================================================
 * FILE: par.c
 *
 * PURPOSE:  set of routines to provide simple access to a R/O parameter file
 *   slightly modeled after FORTRAN namelist.  Parameters can also be read
 *   directly from the commandline in the same format.
 *
 * EXAMPLE of input file in 'par' format:
 *       <blockname1>      # block name; should be on a line by itself
 *       name1 = value1    # whitespace around the = is optional
 *                         # blank lines between blocks are OK
 *       <blockname2>      # start new block
 *       name1 = value1    # note that name1 can appear in different blocks
 *       name2 = value2    #
 *
 *       <blockname1>      # same blockname can re-appear, though is a bit odd
 *       name3 = value3    # this would be the 3rd name in this block
 *
 * LIMITATIONS:
 *   - MAXLEN means static strings; could make fancier dynamic string reader
 *   - blocknames and parameters (key=val#comment) are all single-line based
 *
 * HISTORY:
 *   15-nov-2002  Created for the Athena/Cambridge release 1.0 -- Peter Teuben
 *   18-nov-2002  added par_cmdline()                               -- PJT
 *   6-jan-2003   made the public par_getX() routines               -- PJT
 *   6-jan-2003   Implemented par_setX() routines -- Thomas A. Gardiner
 *   7-jan-2003   add_block() find or add a named block             -- TAG
 *   7-jan-2003   add_par() split into add_par() and add_par_line() -- TAG
 *   7-jan-2003   mode = 0, par_dump() outputs the comment field    -- TAG
 *   9-jan-2003   add_par() only overwrites a comment field in an 
 *                    existing par if the input comment is != NULL. -- TAG
 *   9-jan-2003   Column aligned format added to mode=0, par_dump() -- TAG
 *   1-mar-2004   Moved error out to utils.c as ath_error()         -- PJT
 *   2-apr-2004   Added the get_par_def routines                    -- PJT
 *
 * CONTAINS PUBLIC FUNCTIONS:
 *   int par_open()        - open and read a parameter file for R/O access
 *   void par_cmdline()    - parse a commandline, extract parameters
 *   int par_exist()       - returns 0 if block/name exists
 *   char *par_gets()      - returns a string from input field
 *   int par_geti()        - returns an integer from the input field
 *   double par_getd()     - returns a Real from the input field
 *   char *par_gets_def()  - set string to input value, or default
 *   int par_geti_def()    - set int to input value, or default
 *   double par_getd_def() - set double to input value, or default
 *   void par_sets()       - sets/adds a string
 *   void par_seti()       - sets/adds an integer
 *   void par_setd()       - sets/adds a Real
 *   void par_dump()       - print out all Blocks/Pars for debugging
 *   void par_close()      - free memory
 *   void par_dist_mpi()   - broadcast Blocks and Pars to children in MPI 
 *
 * VARIABLE TYPE AND STRUCTURE DEFINITIONS:
 *   Par_s   - linked list of parameters
 *   Block_s - linked list of Pars
 *============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include "header/chemistry.h"
#include "header/prototypes.h"
#include "header/chemproto.h"

static int  now_open = 0;         /* keep track of status of the par routines */
static char *now_filename = NULL; /* keep pointer to last open filename */

/* Par_s: holds a single name=value#comment tuple as a linked list */
typedef struct Par_s {   
  char *name;                 /* name of the parameter */
  char *value;                /* (string) value of the parameter */
  char *comment;              /* space for comment */
  struct Par_s *next;         /* pointer to the next parameter */
} Par;

/* linked list of Pars that belong together */
typedef struct Block_s { 
  char *name;                 /* name of this block */
  Par  *p;                    /* first member of list in this block */
  int max_name_len;           /* length of longest name  in Par list */
  int max_value_len;          /* length of longest value in Par list */
  struct Block_s *next;       /* pointer to next block */
} Block;

typedef void (*proc)(void);        /* function pointer type */
static Block *base_block = NULL;   /* base of all Block's that contain Par's  */
static int debug = 0;              /* debug level, set to 1 for debug output  */

/*==============================================================================
 * PRIVATE FUNCTION PROTOTYPES: 
 *   allocate()       - wrapper for calloc which terminates code on failure
 *   my_strdup()      - wrapper for strdup which terminates code on failure
 *   skipwhite()      - returns pointer to next non-whitespace
 *   str_term()       - remove whitespace at end of string
 *   line_block_name()- extract block name
 *   add_par()        - add "name = value # comment" to Par list
 *   add_par_line()   - parse line and add it to a block
 *   add_block()      - find or add a new named block
 *   find_block()     - check if a Block name already exists
 *   find_par()       - check if a Block contains a Par with certain name
 *   free_all         - free all Blocks/Pars
 *   par_getsl        - return string, for use of local functions only
 *   par_debug()      - sets debug level in test program
 *============================================================================*/

static void *allocate(size_t size);
static char *my_strdup(char *in);
static char *skipwhite(char *cp);
static void str_term(char *cp);
static char *line_block_name(char *line);
static void add_par(Block *bp, char *name, char *value, char *comment);
static void add_par_line(Block *bp, char *line);
static Block *add_block(char *name);
static Block *find_block(char *name);
static Par *find_par(Block *bp, char *name);
static void free_all(void);
static char *par_getsl(char *block, char *name);
void par_debug(int level);

/*=========================== PUBLIC FUNCTIONS ===============================*/
/*----------------------------------------------------------------------------*/
/* par_open:  open a parameter file for R/O access.  Lines read from the file
 *   are locally patched; all names, values and comments are allocated and put
 *   into a linked list of Block's and Par's.
 */

void par_open(char *filename)
{
  FILE *fp;
  char line[MAXLEN];
  char *block_name, *cp;
  Block *bp = NULL;

  if (now_open) ath_error("Parameter file %s still open\n",now_filename);
  if (now_filename) free(now_filename);
  fp = fopen(filename,"r");
  if (fp == NULL) 
    ath_error("Parameter file %s could not be opened, try -i PARFILE\n"
      ,filename);
  if (debug) fprintf(stdout,"Opening \"%s\" for parameter access\n",filename);
  now_open = 1;
  now_filename = my_strdup(filename);

  while (fgets(line,MAXLEN,fp)) {      /* dumb approach: one line per command */
    cp = skipwhite(line);              /* skip whitespace first */
    if (*cp == '\0') continue;         /* blank line */
    if (*cp == '#') continue;          /* skip comments */
    if (strncmp(cp,"<par_end>",9) == 0) break; /* end of parameter file */
    if (*cp == '<') {                   /* a new Block */
      block_name = line_block_name(cp); /* extract name from <name> */
      bp = add_block(block_name);       /* find or add a block with this name */
      continue;
    }
    add_par_line(bp,cp);                /* add par to this block */
  }
  fclose(fp);
  return;
}

/*----------------------------------------------------------------------------*/
/* par_cmdline: parse a commandline, very forgiving (no warnings) when not in
 *   the right block/name=value format */

void par_cmdline(int argc, char *argv[])
{
  int i;
  char *sp, *ep;
  char *block, *name, *value;
  Block *bp;
  Par *pp;
  int len;

  if (debug) printf("PAR_CMDLINE: \n");
  for (i=1; i<argc; i++) {
    block = argv[i];
    sp = strchr(block,'/');
    if ((sp = strchr(block,'/')) == NULL) continue;
    *sp = '\0';
    name = sp + 1;

    if((ep = strchr(name,'=')) == NULL){
      *sp = '/'; /* Repair argv[i] */
      continue;
    }
    *ep = '\0';
    value = ep + 1;

    if (debug) printf("PAR_CMDLINE: %s/%s=%s\n",block,name,value);
    bp = find_block(block);
    if (bp == NULL) ath_error("par_cmdline: Block \"%s\" not found\n",block);
    pp = find_par(bp,name);
    if (pp == NULL) ath_error("par_cmdline: Par \"%s\" not found\n",name);
    free(pp->value);
    pp->value = my_strdup(value);

    len = (int)strlen(value); /* Update the maximum Par value length */
    bp->max_value_len = len > bp->max_value_len ? len : bp->max_value_len;

/* Repair argv[i] */
    *sp = '/';
    *ep = '=';
  }
}

/*----------------------------------------------------------------------------*/
/* par_exist: return 0 or 1 if a block/name exists */

int par_exist(char *block, char *name)
{
  Block *bp;
  Par *pp;

  if (!now_open) ath_error("par_exist: No open parameter file\n");
  if (block == NULL) ath_error("par_exist: no block name specified\n");
  if (name  == NULL) ath_error("par_exist: no par name specified\n");
  bp = find_block(block);
  if (bp == NULL) return 0;
  pp = find_par(bp,name);
  return (pp == NULL ? 0 : 1);
}

/*----------------------------------------------------------------------------*/
/* par_gets:  return a string */

char  *par_gets(char *block, char *name)
{
  char *pp = par_getsl(block,name);
  return my_strdup(pp);
}

/*----------------------------------------------------------------------------*/
/* par_geti:  return an integer */

int    par_geti(char *block, char *name)
{
  char *pp = par_getsl(block,name);
  return atoi(pp);
}

/*----------------------------------------------------------------------------*/
/* par_getd:  return a Real value */

double par_getd(char *block, char *name)
{
  char *pp = par_getsl(block,name);
  return atof(pp);
}

/*----------------------------------------------------------------------------*/
/* par_gets_def:  return string *name in *block if it exists, else use
 *   the string *def as a default value */

char  *par_gets_def(char *block, char *name, char *def)
{
  if (par_exist(block,name)) {
    char *pp = par_getsl(block,name);
    return my_strdup(pp);
  }

  par_sets(block,name,def,"Default Value");
  return my_strdup(def);
}

/*----------------------------------------------------------------------------*/
/* par_geti_def:  return integer *name in *block if it exists, else use
 *   the integer def as a default value */

int  par_geti_def(char *block, char *name, int def)
{
  if (par_exist(block,name)) {
    char *pp = par_getsl(block,name);
    return atoi(pp);
  }

  par_seti(block,name,"%d",def,"Default Value");
  return def;
}

/*----------------------------------------------------------------------------*/
/* par_getd_def:  return double *name in *block if it exists, else use
 *   the double def as a default value  */

double par_getd_def(char *block, char *name, double def)
{
  if (par_exist(block,name)) {
    char *pp = par_getsl(block,name);
    return atof(pp);
  }

  par_setd(block,name,"%.15e",def,"Default Value");
  return def;
}

/*----------------------------------------------------------------------------*/
/* par_sets: set or add a string */

void par_sets(char *block, char *name, char *sval, char *comment)
{
  Block *bp = add_block(block);  /* find or add a block with this name */

  add_par(bp,name,sval,comment); /* Add the name = value pair Parameter */
  return;
}

/*----------------------------------------------------------------------------*/
/* par_seti: set or add an integer */

void par_seti(char *block, char *name, char *fmt, int ival, char *comment)
{
  Block *bp = add_block(block);  /* find or add a block with this name */
  char sval[MAXLEN];

  sprintf(sval,fmt,ival);
  add_par(bp,name,sval,comment); /* Add the name = value pair Parameter */
  return;
}

/*----------------------------------------------------------------------------*/
/* par_setd: set or add a double */

void par_setd(char *block, char *name, char *fmt, double dval, char *comment)
{
  Block *bp = add_block(block);  /* find or add a block with this name */
  char sval[MAXLEN];

  sprintf(sval,fmt,dval);
  add_par(bp,name,sval,comment); /* Add the name = value pair Parameter */
  return;
}

/*----------------------------------------------------------------------------*/
/* par_dump: debugging aid: print out the current status of all Blocks/Pars */

void par_dump(int mode, FILE *fp)
{
  char fmt[80];
  Block *bp;
  Par *pp;

  if(mode != 2)
    fprintf(fp,"# --------------------- PAR_DUMP -----------------------\n\n");

  for(bp = base_block; bp != NULL; bp = bp->next){
    if (mode == 1) 
      fprintf(fp,"%s::\n",bp->name);
    else{
      fprintf(fp,"<%s>\n",bp->name);
      sprintf(fmt,"%%-%ds = %%-%ds",bp->max_name_len,bp->max_value_len);
    }

    for(pp = bp->p; pp != NULL; pp = pp->next){
      if (mode == 1)
	fprintf(fp," %s/%s = %s\n",bp->name,pp->name,pp->value);
      else{
	fprintf(fp,fmt,pp->name,pp->value);
	if(pp->comment == NULL) fprintf(fp,"\n");
	else fprintf(fp," # %s\n",pp->comment);
      }
    }
    fputc('\n',fp);
  }

  if(mode == 2)
    fprintf(fp,"<par_end>\n");
  else
    fprintf(fp,"# --------------------- PAR_DUMP -------------------------\n");

  return;
}

/*----------------------------------------------------------------------------*/
/* par_close:  close up shop, free memory  */

void par_close(void)
{
  if (!now_open){
    fprintf(stderr,"[par_close]: No open parameter file\n");
    return;
  }
  now_open = 0;
  free_all();
}

/*=========================== PRIVATE FUNCTIONS ==============================*/

/*--------------------------------------------------------------------------- */
/* allocate: handy to call if you want to be certain to get the memory, 
 *   else it will die here
 */

static void *allocate(size_t size) 
{
  void *mem = calloc(1,size);

  if(mem == NULL) ath_error("allocate: not enough memory for %d bytes\n",size);
  return mem;
}

/*----------------------------------------------------------------------------*/
/*  my_strdup: a simple wrapper for strdup which dies in case of failure.  */

static char *my_strdup(char *in)
{
  char *out = ath_strdup(in);
  if(out == NULL) ath_error("Error allocating memory for a new string\n");
  return out;
}

/*----------------------------------------------------------------------------*/
/* skipwhite : skip whitespace, returning pointer to next location of
 *    non-whitespace, or NULL
 */

static char *skipwhite(char *cp)
{
  while(isspace(*cp))
    cp++;
  return cp;
}

/*----------------------------------------------------------------------------*/
/* str_term : Terminate a string, removing any extra white space at the end
 *   of the string. Input char pointer points to the terminating character,
 *   e.g. '\0'.
 */

static void str_term(char *cp)
{
  do{                           /* Remove any trailing white space */
    cp--;
  }while(isspace(*cp));

  *(++cp) = '\0';               /* Terminate the string */

  return;
}

/*----------------------------------------------------------------------------*/
/* line_block_name:  extract a block name from a line containing <block>
 *   Note it returns pointer into a patched piece of the input 'line'
 */

static char *line_block_name(char *line) 
{
  char *sp, *cp;

/* We assume that (*line == '<') is true */
/* Skip leading white space and remember the start of block name */
  sp = cp = skipwhite(line+1);
  while (*cp != '\0' && *cp != '>')
    cp++;
  if (*cp != '>') ath_error("Blockname %s does not appear terminated\n",sp);
  str_term(cp);      /* patch the line and remove any trailing white space */

  return sp;         /* return pointer into the now patched input string */
}

/*----------------------------------------------------------------------------*/
/* add_par: Add a name = value # comment set to the Par list in the
 *   block *bp.  If a parameter with the input name exists the value is
 *   replaced and if the input comment string is non-NULL it is also overwritten
 */

static void add_par(Block *bp, char *name, char *value, char *comment)
{
  Par *pp;
  Par **pnext;
  int len;

  len = (int)strlen(name); /* Update the maximum Par name length */
  bp->max_name_len  = len > bp->max_name_len  ? len : bp->max_name_len;

  len = (int)strlen(value); /* Update the maximum Par value length */
  bp->max_value_len = len > bp->max_value_len ? len : bp->max_value_len;

  if(debug){
    printf("   add_par: %-16s = %s",name,value);
    if(comment == NULL) printf("\n");
    else printf(" # %s\n",comment);
    printf("   max_name_len  = %d\n   max_value_len = %d\n",
	   bp->max_name_len,bp->max_value_len);
  }

  for(pnext = &(bp->p); (pp = *pnext) != NULL; pnext = &(pp->next)){
    if(strcmp(pp->name,name) == 0){          /* if name already given earlier */
/* Replace the value with the new value */
      free(pp->value);
      pp->value = my_strdup(value);
/* Optionally replace the comment with the new comment */
      if(comment != NULL){
	if(pp->comment != NULL) free(pp->comment);
	pp->comment = my_strdup(comment);
      }
      return;
    }
  }

/* Allocate a new Par and set pp to point to it */
  pp = *pnext = (Par *) allocate(sizeof(Par));

  pp->name = my_strdup(name);   /* save name */
  pp->value = my_strdup(value); /* save value */
  pp->comment = (comment == NULL ? NULL : my_strdup(comment)); /* comment? */
  pp->next = NULL;              /* Terminate the chain */
  return;
}

/*----------------------------------------------------------------------------*/
/* add_par_line:  parse a line, assume it's "key = value # comment" 
 *   and add it into a block
 */

static void add_par_line(Block *bp, char *line)
{
  char *cp;
  char *name, *equal=NULL, *value=NULL, *hash=NULL, *comment=NULL, *nul;

  if(bp == NULL)
    ath_error("[add_par_line]: (no block name) while parsing line \n%s\n",line);

  name = skipwhite(line);           /* name */

  for(cp = name; *cp != '\0'; cp++){/* Find the first '=' and '#' */
    if(*cp == '='){
      if(equal == NULL){
	equal = cp;                 /* store the equals sign location */
	value = skipwhite(cp + 1);  /* value */
      }
    }
    if(*cp == '#'){
      hash = cp;                    /* store the hash sign location */
      comment = skipwhite(cp + 1);  /* comment */
      break;
    }
  }

  while(*cp != '\0') cp++;          /* Find the NUL terminator */
  nul = cp;

  if(equal == NULL)
    ath_error("No '=' found in line \"%s\"\n",line);

  str_term(equal);                  /* Terminate the name string */

  if(hash == NULL){
    str_term(nul);                  /* Terminate the value string */
  }
  else{
    str_term(hash);                 /* Terminate the value string */

    if(*comment == '\0')
      comment = NULL;               /* Comment field is empty */
    else
      str_term(nul);                /* Terminate the comment string */
  }

  add_par(bp,name,value,comment);
}

/*----------------------------------------------------------------------------*/
/* add_block:  find or add a new named Block */

static Block *add_block(char *name)
{
  Block *bp;
  Block **pnext;

  if (debug) printf("add_block: %s\n",name);

  for(pnext = &base_block; (bp = *pnext) != NULL; pnext = &(bp->next)){
    if(strcmp(bp->name,name) == 0) return bp;
  }
/* Allocate a new Block and set bp to point to it */
  bp = *pnext = (Block *) allocate(sizeof(Block));

  bp->name = my_strdup(name); /* Store the Block name */
  bp->p = NULL;               /* Terminate the Par list */
  bp->max_name_len  = 0;      /* no Pars - no names */
  bp->max_value_len = 0;      /* no Pars - no values */
  bp->next = NULL;            /* Terminate the Block list */
  return bp;
}

/*----------------------------------------------------------------------------*/
/* find_block: check if a Block name already exists */

static Block *find_block(char *name)
{
  Block *bp;

  if (debug) printf("find_block: %s\n",name);
  for(bp = base_block; bp != NULL; bp = bp->next){
    if (strcmp(bp->name,name) == 0) return bp;
  }

  return NULL;
}

/*----------------------------------------------------------------------------*/
/* find_par: check if a Block contains a Par with certain name */

static Par *find_par(Block *bp, char *name)
{
  Par *pp;

  if (debug) printf("find_par: %s\n",name);
  for(pp = bp->p; pp != NULL; pp = pp->next){
    if (strcmp(pp->name,name) == 0) return pp;
  }

  return NULL;
}

/*----------------------------------------------------------------------------*/
/* free_all:  free all stuff associated with Block's and Par's */

static void free_all(void)
{
  Block *bp1, *bp;
  Par *pp1, *pp;

  if (now_filename) free(now_filename);
  now_filename = NULL; /* Return to the initial, empty state */

  for(bp = base_block; bp != NULL; bp = bp1){ /* loop over all blocks */
    bp1 = bp->next; /* store the pointer to the next block */
    for(pp = bp->p; pp != NULL; pp = pp1){ /* loop over all pars */
      pp1 = pp->next; /* store the pointer to the next par */
      if (pp->name)    free(pp->name);
      if (pp->value)   free(pp->value);
      if (pp->comment) free(pp->comment);
      free(pp);
    }
    if (bp->name) free(bp->name);
    free(bp);
  }
  base_block = NULL; /* Return to the initial, empty state */
}

/*----------------------------------------------------------------------------*/
/* par_getsl: helper function to return a local string. external clients should
 *   call par_gets, which returns a newline allocate string
 */

static char *par_getsl(char *block, char *name)
{
  Block *bp;
  Par *pp;

  if (!now_open) ath_error("par_gets: No open parameter file\n");
  if (block == NULL) ath_error("par_gets: no block name specified\n");
  if (name  == NULL) ath_error("par_gets: no par name specified\n");
  bp = find_block(block);
  if (bp == NULL) ath_error("par_gets: Block \"%s\" not found\n",block);
  pp = find_par(bp,name);
  if (pp == NULL)
    ath_error("par_gets: Par \"%s\" not found in Block \"%s\"\n",name,block);
  return pp->value;
}

/*----------------------------------------------------------------------------*/
/* par_debug: set debug flag to level.  Call with argument=1 to enable 
 *   diagnositc output.    Alas, not really for the outside world to use */

void par_debug(int level) {      /* un - advertised :-) */
  debug = level;
}
