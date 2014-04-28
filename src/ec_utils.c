/*
    ettercap -- Ettercap utilities

    Copyright (C) ALoR & NaGA

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

*/


#include <ec.h>
#include <ec_interfaces.h>
#include <ec_sniff.h>
#include <ec_send.h>
#include <ec_log.h>
#include <ec_format.h>
#include <ec_mitm.h>
#include <ec_filter.h>
#include <ec_plugins.h>
#include <ec_conf.h>
#include <ec_strings.h>
#include <ec_encryption.h>
#ifdef HAVE_EC_LUA
#include <ec_lua.h>
#endif

#include <ctype.h>

/*
 * This function parses the input in the form [1-3,17,5-11]
 * and fill the structure with expanded numbers.
 */

int expand_token(char *s, u_int max, void (*func)(void *t, u_int n), void *t )
{
   char *str = strdup(s);
   char *p, *q, r;
   char *end;
   u_int a = 0, b = 0;
   
   DEBUG_MSG("expand_token %s", s);
   
   p = str;
   end = p + strlen(p);
   
   while (p < end) {
      q = p;
      
      /* find the end of the first digit */
      while ( isdigit((int)*q) && q++ < end);
      
      r = *q;   
      *q = 0;
      /* get the first digit */
      a = atoi(p);
      if (a > max) 
         FATAL_MSG("Out of range (%d) !!", max);
      
      /* it is a range ? */
      if ( r == '-') {
         p = ++q;
         /* find the end of the range */
         while ( isdigit((int)*q) && q++ < end);
         *q = 0;
         if (*p == '\0') 
            FATAL_MSG("Invalid range !!");
         /* get the second digit */
         b = atoi(p);
         if (b > max) 
            FATAL_MSG("Out of range (%d)!!", max);
         if (b < a)
            FATAL_MSG("Invalid decrementing range !!");
      } else {
         /* it is not a range */
         b = a; 
      } 
      
      /* process the range and invoke the callback */
      for(; a <= b; a++) {
         func(t, a);
      }
      
      if (q == end) break;
      else  p = q + 1;      
   }
  
   SAFE_FREE(str);
   
   return ESUCCESS;
}

/*
 * compile the regex
 */

int set_regex(char *regex)
{
   int err;
   char errbuf[100];
   
   DEBUG_MSG("set_regex: %s", regex);

   /* free any previous compilation */
   if (GBL_OPTIONS->regex)
      regfree(GBL_OPTIONS->regex);

   /* unset the regex if empty */
   if (!strcmp(regex, "")) {
      SAFE_FREE(GBL_OPTIONS->regex);
      return ESUCCESS;
   }
  
   /* allocate the new structure */
   SAFE_CALLOC(GBL_OPTIONS->regex, 1, sizeof(regex_t));
  
   /* compile the regex */
   err = regcomp(GBL_OPTIONS->regex, regex, REG_EXTENDED | REG_NOSUB | REG_ICASE );

   if (err) {
      regerror(err, GBL_OPTIONS->regex, errbuf, sizeof(errbuf));
      FATAL_MSG("%s\n", errbuf);
   }

   return ESUCCESS;
}

char **parse_iflist(char *list)
{
   int i, n;
   char **r, *t, *p;

   for(i = 0, n = 1; list[i] != '\0'; list[i++] == ',' ? n++ : n);
   SAFE_CALLOC(r, n + 1, sizeof(char*));

   /* its self-explaining */
   for(r[i=0]=ec_strtok(list,",",&p);i<n&&(t=ec_strtok(NULL,",",&p))!=NULL;r[++i]=strdup(t));
   r[n] = NULL;

   return r;
}

/*
 * regain root privs inside an atexit call
 */
void regain_privs_atexit(void)
{
   DEBUG_MSG("ATEXIT: regain_privs");
   regain_privs();
}

/*
 * regain root privs
 */
void regain_privs(void)
{

#ifdef OS_WINDOWS
   return;
#endif
   if(seteuid(0) < 0)
      ERROR_MSG("seteuid()");

   USER_MSG("Regained root privileges: %d %d", getuid(), geteuid());
}

/* 
 * drop root privs 
 */
void drop_privs(void)
{
   u_int uid, gid;
   char *var;

#ifdef OS_WINDOWS
   /* do not drop privs under windows */
   return;
#endif

   /* are we root ? */
   if (getuid() != 0)
      return;

   /* get the env variable for the UID to drop privs to */
   var = getenv("EC_UID");

   /* if the EC_UID variable is not set, default to GBL_CONF->ec_uid (nobody) */
   if (var != NULL)
      uid = atoi(var);
   else
      uid = GBL_CONF->ec_uid;

   /* get the env variable for the GID to drop privs to */
   var = getenv("EC_GID");

   /* if the EC_UID variable is not set, default to GBL_CONF->ec_gid (nobody) */
   if (var != NULL)
      gid = atoi(var);
   else
      gid = GBL_CONF->ec_gid;

   DEBUG_MSG("drop_privs: setuid(%d) setgid(%d)", uid, gid);

   /* drop to a good uid/gid ;) */
   if ( setgid(gid) < 0 )
      ERROR_MSG("setgid()");

   if ( seteuid(uid) < 0 )
      ERROR_MSG("seteuid()");

   DEBUG_MSG("privs: UID: %d %d  GID: %d %d", (int)getuid(), (int)geteuid(), (int)getgid(), (int)getegid() );
   USER_MSG("Privileges dropped to UID %d GID %d...\n\n", (int)getuid(), (int)getgid() );
}

#include <openssl/pem.h>
#include <math.h>

int base64encode(const char *b64_encode_me, char** buffer) {
	BIO *bio, *b64;
	FILE* stream;
	int encodedSize = 4*ceil((double)strlen(b64_encode_me)/3);
	*buffer = (char *)malloc(encodedSize+1);
	stream = fmemopen(*buffer, encodedSize+1, "w");
	b64 = BIO_new(BIO_f_base64());
	bio = BIO_new_fp(stream, BIO_NOCLOSE);
	bio = BIO_push(b64, bio);
	BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
	BIO_write(bio, b64_encode_me, strlen(b64_encode_me));
	(void)BIO_flush(bio);
	BIO_free_all(bio);
	fclose(stream);

	return ESUCCESS;
	
}

int get_decode_len(const char *b64_str) {
	int len = strlen(b64_str);
	int padding = 0;
	if (b64_str[len-1] == '=' && b64_str[len-2] == '=')
		padding = 2;
	else if (b64_str[len-1] == '=')
		padding = 1;
	return (int)len*0.75 - padding;
}

int base64decode(const char *decode_me, char** buffer) {
	BIO *bio, *b64;
	int decodeLen = get_decode_len(decode_me), len = 0;

	*buffer = (char *)malloc(decodeLen+1);
	memset(*buffer, '\0', decodeLen+1);
	FILE* stream = fmemopen((void*)decode_me, strlen(decode_me), "r");

	b64 = BIO_new(BIO_f_base64());
	bio = BIO_new_fp(stream, BIO_NOCLOSE);
	bio = BIO_push(b64, bio);
	BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
	len = BIO_read(bio, *buffer, strlen(decode_me));
	if (len != decodeLen)
		return 0;
	(*buffer)[len] = '\0';
	BIO_free_all(bio);
	fclose(stream);

	return decodeLen;
}

/*
 * Return a 'ctime()' time-string from either:
 *   a 'struct timeval *tv'
 * or if 'tv == NULL',
 *   returns a time-value for current time.
 *
 * NOT threadsafe (returns a static buffer), but there should hopefully
 * be no problem (?).
 */
const char *ec_ctime(const struct timeval *tv)
{
   const char *ts_str;
   static char result[30];
   time_t t;

   if (!tv)
      t = time(NULL);
   else
      t = (time_t) tv->tv_sec;

   ts_str = ctime(&t);

   /* ctime() has a newline at position 24. Get rid of it.  */
   if (ts_str)
      sprintf(result, "%.24s", ts_str);
   else
      snprintf(result, sizeof(result), "%lu.%06lu", tv->tv_sec, tv->tv_usec);

  return (result);
}

/* EOF */


// vim:ts=3:expandtab

