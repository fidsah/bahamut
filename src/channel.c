/*
 *   IRC - Internet Relay Chat, src/channel.c
 *   Copyright (C) 1990 Jarkko Oikarinen and
 *                      University of Oulu, Co Center
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 1, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* $Id$ */

#include "struct.h"
#include "common.h"
#include "sys.h"
#include "numeric.h"
#include "channel.h"
#include "h.h"

#ifdef NO_CHANOPS_WHEN_SPLIT
#include "fdlist.h"
extern fdlist serv_fdlist;

int         server_was_split = YES;
time_t      server_split_time = 0;
int         server_split_recovery_time = (MAX_SERVER_SPLIT_RECOVERY_TIME * 60);

#define USE_ALLOW_OP
#endif

#ifdef LITTLE_I_LINES
#ifndef USE_ALLOW_OP
#define USE_ALLOW_OP
#endif
#endif

aChannel   *channel = NullChn;

static void add_invite(aClient *, aChannel *);
static int  add_banid(aClient *, aChannel *, char *);
static int  can_join(aClient *, aChannel *, char *);
static void channel_modes(aClient *, char *, char *, aChannel *);
static int  del_banid(aChannel *, char *);
static Link *is_banned(aClient *, aChannel *);
static int
            set_mode(aClient *, aClient *, aChannel *, int, int,
		     char **, char *, char *);
static void sub1_from_channel(aChannel *);

int check_channelname(aClient *, unsigned char *);
void        clean_channelname(unsigned char *);
void        del_invite(aClient *, aChannel *);

#ifdef ORATIMING
struct timeval tsdnow, tsdthen;
unsigned long tsdms;

#endif

/* number of seconds to add to all readings of time() when making TS's */

static char *PartFmt = ":%s PART %s";
static char *PartFmt2 = ":%s PART %s :%s";

/* some buffers for rebuilding channel/nick lists with ,'s */
static char nickbuf[BUFSIZE], buf[BUFSIZE];
static char modebuf[MODEBUFLEN], parabuf[MODEBUFLEN];

/* htm ... */
extern int lifesux;

/* externally defined function */
extern Link *find_channel_link(Link *, aChannel *);	/* defined in list.c */
#ifdef ANTI_SPAMBOT
extern int  spam_num;		/* defined in s_serv.c */
extern int  spam_time;		/* defined in s_serv.c */
#endif
/* return the length (>=0) of a chain of links. */
static int list_length(Link *lp) {
   Reg int     count = 0;

   for (; lp; lp = lp->next)
      count++;
   return count;
}
/*
 * find_chasing 
 *   Find the client structure for a nick name (user) using history 
 *   mechanism if necessary. If the client is not found, an error message 
 *   (NO SUCH NICK) is generated. If the client was found through the 
 *   history, chasing will be 1 and otherwise 0.
 */
aClient *find_chasing(aClient *sptr, char *user, int *chasing) {
   Reg aClient *who = find_client(user, (aClient *) NULL);

   if (chasing)
      *chasing = 0;
   if (who)
      return who;
   if (!(who = get_history(user, (long) KILLCHASETIMELIMIT))) {
      sendto_one(sptr, err_str(ERR_NOSUCHNICK),
		 me.name, sptr->name, user);
      return ((aClient *) NULL);
   }
   if (chasing)
      *chasing = 1;
   return who;
}

/*
 * Fixes a string so that the first white space found becomes an end of
 * string marker (`\-`).  returns the 'fixed' string or "*" if the
 * string was NULL length or a NULL pointer.
 */
static char * check_string(char *s) {
   static char star[2] = "*";
   char       *str = s;

   if (BadPtr(s))
      return star;

   for (; *s; s++)
      if (isspace(*s)) {
	 *s = '\0';
	 break;
      }

   return (BadPtr(str)) ? star : str;
}
/*
 * create a string of form "foo!bar@fubar" given foo, bar and fubar as
 * the parameters.  If NULL, they become "*".
 */
static char *make_nick_user_host(char *nick, char *name, char *host) {
   static char namebuf[NICKLEN + USERLEN + HOSTLEN + 6];
   int         n;
   Reg char   *ptr1, *ptr2;

   ptr1 = namebuf;
   for (ptr2 = check_string(nick), n = NICKLEN; *ptr2 && n--;)
      *ptr1++ = *ptr2++;
   *ptr1++ = '!';
   for (ptr2 = check_string(name), n = USERLEN; *ptr2 && n--;)
      *ptr1++ = *ptr2++;
   *ptr1++ = '@';
   for (ptr2 = check_string(host), n = HOSTLEN; *ptr2 && n--;)
      *ptr1++ = *ptr2++;
   *ptr1 = '\0';
   return (namebuf);
}
/* Ban functions to work with mode +b */
/* add_banid - add an id to be banned to the channel  (belongs to cptr) */

static int add_banid(aClient *cptr, aChannel *chptr, char *banid) {
   Reg Link   *ban;
   Reg int     cnt = 0;

   if (MyClient(cptr))
	  (void) collapse(banid);
	
   for (ban = chptr->banlist; ban; ban = ban->next) {
      if (MyClient(cptr) && (++cnt >= MAXBANS)) {
			sendto_one(cptr, getreply(ERR_BANLISTFULL), me.name, cptr->name,
						  chptr->chname);
			return -1;
		}
		/* yikes, we were doing all sorts of weird crap here before, now
		 * we ONLY want to know if current bans cover this ban, not if this
		 * ban covers current ones, since it may cover other things too -wd */
		else if (!match(ban->value.banptr->banstr, banid))
		  return -1;
   }

   ban = make_link();
   memset((char *) ban, '\0', sizeof(Link));

   ban->flags = CHFL_BAN;
   ban->next = chptr->banlist;

   ban->value.banptr = (aBan *) MyMalloc(sizeof(aBan));
   ban->value.banptr->banstr = (char *) MyMalloc(strlen(banid) + 1);
   (void) strcpy(ban->value.banptr->banstr, banid);

   if (IsPerson(cptr)) {
      ban->value.banptr->who =
	 (char *) MyMalloc(strlen(cptr->name) +
			   strlen(cptr->user->username) +
			   strlen(cptr->user->host) + 3);
      (void) sprintf(ban->value.banptr->who, "%s!%s@%s",
		  cptr->name, cptr->user->username, cptr->user->host);
   }
   else {
      ban->value.banptr->who = (char *) MyMalloc(strlen(cptr->name) + 1);
      (void) strcpy(ban->value.banptr->who, cptr->name);
   }

   ban->value.banptr->when = timeofday;

   chptr->banlist = ban;
   return 0;
}

/*
 * del_banid - delete an id belonging to cptr if banid is null,
 * deleteall banids belonging to cptr.
 */
static int
del_banid(aChannel *chptr, char *banid)
{
   Reg Link  **ban;
   Reg Link   *tmp;

   if (!banid)
      return -1;
   for (ban = &(chptr->banlist); *ban; ban = &((*ban)->next))
      if (mycmp(banid, (*ban)->value.banptr->banstr) == 0)
      {
	 tmp = *ban;
	 *ban = tmp->next;
	 MyFree(tmp->value.banptr->banstr);
	 MyFree(tmp->value.banptr->who);
	 MyFree(tmp->value.banptr);
	 free_link(tmp);
	 break;
      }
   return 0;
}
/*
 * is_banned - returns a pointer to the ban structure if banned else
 * NULL
 * 
 * IP_BAN_ALL from comstud always on...
 */

static Link *is_banned(aClient *cptr, aChannel *chptr) {
   Reg Link   *tmp;
   char        s[NICKLEN + USERLEN + HOSTLEN + 6];
   char       *s2;

   if (!IsPerson(cptr))
      return ((Link *) NULL);

   strcpy(s, make_nick_user_host(cptr->name, cptr->user->username,
				 cptr->user->host));
   s2 = make_nick_user_host(cptr->name, cptr->user->username,
			    cptr->hostip);

   for (tmp = chptr->banlist; tmp; tmp = tmp->next)
      if ((match(tmp->value.banptr->banstr, s) == 0) ||
	  (match(tmp->value.banptr->banstr, s2) == 0))
	 break;
   return (tmp);
}
/*
 * adds a user to a channel by adding another link to the channels
 * member chain.
 */
static void add_user_to_channel(aChannel *chptr, aClient *who, int flags) {
   Reg Link   *ptr;

   if (who->user) {
      ptr = make_link();
      ptr->flags = flags;
      ptr->value.cptr = who;
      ptr->next = chptr->members;
      chptr->members = ptr;
      chptr->users++;

      ptr = make_link();
      ptr->value.chptr = chptr;
      ptr->next = who->user->channel;
      who->user->channel = ptr;
      who->user->joined++;
   }
}

void remove_user_from_channel(aClient *sptr, aChannel *chptr) {
   Reg Link  **curr;
   Reg Link   *tmp;

   for (curr = &chptr->members; (tmp = *curr); curr = &tmp->next)
      if (tmp->value.cptr == sptr) {
	 *curr = tmp->next;
	 free_link(tmp);
	 break;
      }
   for (curr = &sptr->user->channel; (tmp = *curr); curr = &tmp->next)
      if (tmp->value.chptr == chptr) {
	 *curr = tmp->next;
	 free_link(tmp);
	 break;
      }
   sptr->user->joined--;
   sub1_from_channel(chptr);
}

/*
static void change_chan_flag(Link *lp, aChannel *chptr) {
   Reg Link   *tmp;

   if ((tmp = find_user_link(chptr->members, lp->value.cptr))) {
	  if (lp->flags & MODE_ADD) {
		  tmp->flags |= lp->flags & MODE_FLAGS;
		  if (lp->flags & MODE_CHANOP)
			 tmp->flags &= ~MODE_DEOPPED;
	  }
	}
	else
	  tmp->flags &= ~lp->flags & MODE_FLAGS;
}
*/

/*
static void set_deopped(Link *lp, aChannel *chptr) {
   Reg Link   *tmp;

   if ((tmp = find_user_link(chptr->members, lp->value.cptr)))
      if ((tmp->flags & MODE_CHANOP) == 0)
	 tmp->flags |= MODE_DEOPPED;
}
*/

int is_chan_op(aClient *cptr, aChannel *chptr) {
   Reg Link   *lp;

   if (chptr)
      if ((lp = find_user_link(chptr->members, cptr)))
	 return (lp->flags & CHFL_CHANOP);

   return 0;
}

int is_deopped(aClient *cptr, aChannel *chptr) {
   Reg Link   *lp;

   if (chptr)
      if ((lp = find_user_link(chptr->members, cptr)))
	 return (lp->flags & CHFL_DEOPPED);

   return 0;
}

int has_voice(aClient *cptr, aChannel *chptr) {
   Reg Link   *lp;

   if (chptr)
      if ((lp = find_user_link(chptr->members, cptr)))
	 return (lp->flags & CHFL_VOICE);

   return 0;
}

int can_send(aClient *cptr, aChannel *chptr) {
   Reg Link   *lp;
   Reg int     member;

   if (IsServer(cptr) || IsULine(cptr))
      return 0;

   member = IsMember(cptr, chptr);
   lp = find_user_link(chptr->members, cptr);

   if (chptr->mode.mode & MODE_MODERATED &&
       (!lp || !(lp->flags & (CHFL_CHANOP | CHFL_VOICE))))
      return (MODE_MODERATED);

   if (chptr->mode.mode & MODE_NOPRIVMSGS && !member)
      return (MODE_NOPRIVMSGS);

   return 0;
}

aChannel   *find_channel(char *chname, aChannel *chptr) {
   return hash_find_channel(chname, chptr);
}
/*
 * write the "simple" list of channel modes for channel chptr onto
 * buffer mbuf with the parameters in pbuf.
 */
static void
channel_modes(aClient *cptr, char *mbuf, char *pbuf, aChannel *chptr) {
   *mbuf++ = '+';
   if (chptr->mode.mode & MODE_SECRET)
      *mbuf++ = 's';
   else if (chptr->mode.mode & MODE_PRIVATE)
      *mbuf++ = 'p';
   if (chptr->mode.mode & MODE_MODERATED)
      *mbuf++ = 'm';
   if (chptr->mode.mode & MODE_TOPICLIMIT)
      *mbuf++ = 't';
   if (chptr->mode.mode & MODE_INVITEONLY)
      *mbuf++ = 'i';
   if (chptr->mode.mode & MODE_NOPRIVMSGS)
      *mbuf++ = 'n';
   if (chptr->mode.mode & MODE_REGISTERED)
      *mbuf++ = 'r';
   if (chptr->mode.mode & MODE_REGONLY)
      *mbuf++ = 'R';
   if (chptr->mode.limit) {
      *mbuf++ = 'l';
      if (IsMember(cptr, chptr) || IsServer(cptr) || IsULine(cptr))
	 (void) ircsprintf(pbuf, "%d ", chptr->mode.limit);
   }
   if (*chptr->mode.key) {
      *mbuf++ = 'k';
      if (IsMember(cptr, chptr) || IsServer(cptr) || IsULine(cptr))
	 (void) strcat(pbuf, chptr->mode.key);
   }
   *mbuf++ = '\0';
   return;
}

static void
send_mode_list(aClient *cptr, aChannel *chptr,
	       char *chname,
	       Link *top,
	       int mask,
	       char flag)
{
   Reg Link   *lp;
   Reg char   *cp, *name;
   int         count = 0, send = 0;

   cp = modebuf + strlen(modebuf);
   if (*parabuf)		/*
				 * mode +l or +k xx 
				 */
      count = 1;
   for (lp = top; lp; lp = lp->next) {
      if (!(lp->flags & mask))
	 continue;
      if (mask == CHFL_BAN)
	 name = lp->value.banptr->banstr;
      else
	 name = lp->value.cptr->name;
      if (strlen(parabuf) + strlen(name) + 10 < (size_t) MODEBUFLEN) {
	 (void) strcat(parabuf, " ");
	 (void) strcat(parabuf, name);
	 count++;
	 *cp++ = flag;
	 *cp = '\0';
      }
      else if (*parabuf)
	 send = 1;
      if (count == 3)
	 send = 1;
      if (send) {
			sendto_one(cptr, ":%s MODE %s %s %s",
						  me.name, chname, modebuf, parabuf);
	 send = 0;
	 *parabuf = '\0';
	 cp = modebuf;
	 *cp++ = '+';
	 if (count != 3) {
	    (void) strcpy(parabuf, name);
	    *cp++ = flag;
	 }
	 count = 0;
	 *cp = '\0';
      }
   }
}

/* send "cptr" a full list of the modes for channel chptr. */
void
send_channel_modes(aClient *cptr, aChannel *chptr)
{
   Link       *l, *anop = NULL, *skip = NULL;
   int         n = 0;
   char       *t;

   if (*chptr->chname != '#')
      return;

   *modebuf = *parabuf = '\0';
   channel_modes(cptr, modebuf, parabuf, chptr);

   sprintf(buf, ":%s SJOIN %ld %ld %s %s %s :", me.name,
	   chptr->channelts, chptr->creationtime, chptr->chname, modebuf, parabuf);
   t = buf + strlen(buf);
   for (l = chptr->members; l && l->value.cptr; l = l->next)
      if (l->flags & MODE_CHANOP) {
	 anop = l;
	 break;
      }
   /*
    * follow the channel, but doing anop first if it's defined *
    * -orabidoo
    */
   l = NULL;
   for (;;) {
      if (anop) {
	 l = skip = anop;
	 anop = NULL;
      }
      else {
	 if (l == NULL || l == skip)
	    l = chptr->members;
	 else
	    l = l->next;
	 if (l && l == skip)
	    l = l->next;
	 if (l == NULL)
	    break;
      }
      if (l->flags & MODE_CHANOP)
	 *t++ = '@';
      if (l->flags & MODE_VOICE)
	 *t++ = '+';
      strcpy(t, l->value.cptr->name);
      t += strlen(t);
      *t++ = ' ';
      n++;
      if (t - buf > BUFSIZE - 80) {
	 *t++ = '\0';
	 if (t[-1] == ' ')
	    t[-1] = '\0';
	 sendto_one(cptr, "%s", buf);
	 sprintf(buf, ":%s SJOIN %ld %ld %s 0 :",
				me.name, chptr->channelts,
				chptr->creationtime,
				chptr->chname);
	 t = buf + strlen(buf);
	 n = 0;
      }
   }

   if (n) {
      *t++ = '\0';
      if (t[-1] == ' ')
	 t[-1] = '\0';
      sendto_one(cptr, "%s", buf);
   }
   *parabuf = '\0';
   *modebuf = '+';
   modebuf[1] = '\0';
   send_mode_list(cptr, chptr, chptr->chname, chptr->banlist, CHFL_BAN,
		  'b');
   if (modebuf[1] || *parabuf)
      sendto_one(cptr, ":%s MODE %s %s %s",
		 me.name, chptr->chname, modebuf, parabuf);
}
/* m_mode parv[0] - sender parv[1] - channel */

int dont_send_ts_with_mode;

int
m_mode(aClient *cptr,
       aClient *sptr,
       int parc,
       char *parv[])
{
   int         mcount = 0, chanop=0;
   aChannel   *chptr;

   /* Now, try to find the channel in question */
   if (parc > 1) {
      chptr = find_channel(parv[1], NullChn);
      if (chptr == NullChn)
	 return m_umode(cptr, sptr, parc, parv);
   }
   else {
      sendto_one(sptr, err_str(ERR_NEEDMOREPARAMS),
		 me.name, parv[0], "MODE");
      return 0;
   }
	if(!check_channelname(sptr, (unsigned char *) parv[1]))
	  return 0;
   clean_channelname((unsigned char *) parv[1]);
   if(is_chan_op(sptr, chptr) || (IsServer(sptr) && chptr->channelts!=0))
	  chanop=1;
	else if(IsULine(sptr) || (IsSAdmin(sptr) && !MyClient(sptr)))
	  chanop=2; /* extra speshul access */
	
	
   if (parc < 3) {
      *modebuf = *parabuf = '\0';
      modebuf[1] = '\0';
      channel_modes(sptr, modebuf, parabuf, chptr);
      sendto_one(sptr, rpl_str(RPL_CHANNELMODEIS), me.name, parv[0],
		 chptr->chname, modebuf, parabuf);
      sendto_one(sptr, rpl_str(RPL_CREATIONTIME), me.name, parv[0],
		 chptr->chname, chptr->creationtime);
      return 0;
   }

	mcount = set_mode(cptr, sptr, chptr, chanop, parc - 2, parv + 2,
							  modebuf, parabuf);

   if (strlen(modebuf) > (size_t) 1)
      switch (mcount) {
	 case 0:
	    break;
	 case -1:
	    if (MyClient(sptr))
	       sendto_one(sptr,
			  err_str(ERR_CHANOPRIVSNEEDED),
			  me.name, parv[0], chptr->chname);
	    else {
	       ircstp->is_fake++;
	    }
	    break;
	 default:
			sendto_channel_butserv(chptr, sptr,
										  ":%s MODE %s %s %s", parv[0],
										  chptr->chname, modebuf,
										  parabuf);
	    sendto_match_servs(chptr, cptr,
			       ":%s MODE %s %s %s",
			       parv[0], chptr->chname,
			       modebuf, parabuf);
      }
   return 0;
}

/* the old set_mode was pissing me off with it's disgusting
 * hackery, so I rewrote it.  Hope this works. }:> --wd
 */
static int
set_mode(aClient *cptr, aClient *sptr, aChannel *chptr, int level, int parc,
			char *parv[], char *mbuf, char *pbuf) {
	static int flags[] = {
		MODE_PRIVATE, 'p', MODE_SECRET, 's',
      MODE_MODERATED, 'm', MODE_NOPRIVMSGS, 'n',
      MODE_TOPICLIMIT, 't', MODE_REGONLY, 'R',
		MODE_INVITEONLY, 'i',
		0x0, 0x0};
	
	Link *lp; /* for walking lists */
	char *modes=parv[0]; /* user's idea of mode changes */
   int args; /* counter for what argument we're on */
	int banlsent = 0; /* Only list bans once in a command. */
	char change='+'; /* by default we + things... */
	int errors=0; /* errors returned, set with bitflags so we only 
								* return them once */
#define SM_ERR_NOPRIVS 0x0001 /* is not an op */
#define SM_ERR_MOREPARMS 0x0002 /* needs more parameters */	
#define SM_ERR_RESTRICTED 0x0004 /* not allowed to op others or be op'd */
	
#define SM_MAXMODES 6
	/* from remote servers, ungodly numbers of modes can be sent, but
	 * from local users only SM_MAXMODES are allowed */
	int maxmodes=((IsServer(sptr) || IsULine(sptr)) ? 512 : SM_MAXMODES);
	int nmodes=0; /* how many modes we've set so far */
	aClient *who=NULL; /* who we're doing a mode for */
	int chasing = 0;
	int len=0, i=0; /* so we don't overrun pbuf */
	char moreparmsstr[]="MODE   ";
	
	args=1;
	
	if(parc<1)
	  return 0;

	*pbuf=0;
	*mbuf++='+'; /* add the plus, even if they don't */
	/* go through once to clean the user's mode string so we can
	 * have a simple parser run through it...*/
	while(*modes && (nmodes<maxmodes)) {
		switch(*modes) {
		 case '+':
			if(*(mbuf-1)=='-') {
				*(mbuf-1)='+'; /* change it around now */
				change='+';
				break;
			}
			else if(change=='+') /* we're still doing a +, we don't care */
			  break;
			change=*modes;
			*mbuf++='+';
			break;
		 case '-':
			if(*(mbuf-1)=='+') {
				*(mbuf-1)='-'; /* change it around now */
				change='-';
				break;
			}
			else if(change=='-')
			  break; /* we're still doing a -, we don't care */
			change=*modes;
			*mbuf++='-';
			break;
		 case 'o':
		 case 'v':
			if(level<1) {
				errors|=SM_ERR_NOPRIVS;
				break;
			}
			if(parv[args]==NULL) {
				/* enough people complained about this that I took it out
				 * we just silently drop the spare +o's now */
				/*	errors|=SM_ERR_MOREPARMS; */
				break;
			}
			
			who=find_chasing(sptr, parv[args], &chasing);
			lp=find_user_link(chptr->members, who);
			if(lp==NULL) {
				sendto_one(sptr, err_str(ERR_USERNOTINCHANNEL),
							  me.name, cptr->name, parv[args], chptr->chname);
				/* swallow the arg */
				args++;
				break;
			}
#ifdef LITTLE_I_LINE
			if(IsRestricted(sptr) && (change=='+' && *modes=='o')) {
				errors|=SM_ERR_RESTRICTED;
				args++;
				break;
			}
#endif

			/* if we're going to overflow our mode buffer,
			 * drop the change instead */
			i=strlen(lp->value.cptr->name);
			if(len+i>MODEBUFLEN) {
				args++;
				break;
			}
#ifdef LITTLE_I_LINE
			if(MyClient(who) && IsRestricted(who) && (change=='+' && *modes=='o')) {
				/* pass back to cptr a MODE -o to avoid desynch */
				sendto_one(cptr, ":%s MODE %s -o %s", me.name,
							  chptr->chname, who->name);
				sendto_one(who, ":%s NOTICE %s :*** Notice -- %s attempted to chanop you. You are restricted and cannot be chanopped",
							  me.name, who->name, sptr->name);
				sendto_one(sptr, ":%s NOTICE %s :*** Notice -- %s is restricted and cannot be chanopped",
							  me.name, sptr->name, who->name);
				args++;
				break;
			}
#endif

			/* if we have the user, set them +/-[vo] */
			if(change=='+')
			  lp->flags|=(*modes=='o' ? CHFL_CHANOP : CHFL_VOICE);
			else
			  lp->flags&=~((*modes=='o' ? CHFL_CHANOP : CHFL_VOICE));
			/* we've decided their mode was okay, cool */
			*mbuf++=*modes;
			strcat(parabuf, lp->value.cptr->name);
			strcat(parabuf, " ");
			len+=i+1;
			args++;
			nmodes++;
			if (IsServer(sptr) && *modes == 'o' && change=='+') {
				chptr->channelts = 0;
				ts_warn("Server %s setting +o and blasting TS on %s", sptr->name,
						  chptr->chname);
			}
			break;
		 case 'b':
			/* if the user has no more arguments, then they just want
			 * to see the bans, okay, cool. */
			if(level<1 && parv[args]!=NULL) {
				errors|=SM_ERR_NOPRIVS;
				break;
			}
			/* show them the bans, woowoo */
			if(parv[args]==NULL) {
				if (banlsent)
				     break; /* Send only once */
				for(lp=chptr->banlist;lp;lp=lp->next)
				  sendto_one(sptr, rpl_str(RPL_BANLIST), me.name, cptr->name,
								 chptr->chname, lp->value.banptr->banstr,
								 lp->value.banptr->who, lp->value.banptr->when);
				sendto_one(cptr, rpl_str(RPL_ENDOFBANLIST),
							  me.name, cptr->name, chptr->chname);
				banlsent = 1;
				break; /* we don't pass this along, either.. */
			}
			/* do not allow : in bans */
			if(*parv[args]==':') {
				args++;
				break;
			}
			/* make a 'pretty' ban mask here, then try and set it */
			parv[args]=collapse(pretty_mask(parv[args]));
			/* if we're going to overflow our mode buffer,
			 * drop the change instead */
			i=strlen(parv[args]);
			if(len+i>MODEBUFLEN) {
				args++;
				break;
			}
			/* if we can't add or delete (depending) the ban, change is
			 * worthless anyhow */
			if(!(change=='+' && !add_banid(sptr, chptr, parv[args])) && 
				!(change=='-' && !del_banid(chptr, parv[args])))
			  break;
				
			*mbuf++='b';
			strcat(pbuf, parv[args]);
			strcat(pbuf, " ");
			len+=i+1;
			args++;
			nmodes++;
			break;
		 case 'l':
			/* if the user has no more arguments, then they just want
			 * to see the bans, okay, cool. */
			if(level<1) {
				errors|=SM_ERR_NOPRIVS;
				break;
			}
			/* if it's a -, just change the flag, we have no arguments */
			if(change=='-') {
				*mbuf++='l';
				chptr->mode.mode&=~MODE_LIMIT;
				chptr->mode.limit=0;
				nmodes++;
				break;
			}
			else {
				if(parv[args]==NULL) {
					errors|=SM_ERR_MOREPARMS;
					break;
				}
				/* if we're going to overflow our mode buffer,
				 * drop the change instead */
				i=strlen(parv[args]);
				if(len+i>MODEBUFLEN) {
					args++;
					break;
				}
				chptr->mode.mode|=MODE_LIMIT;
				chptr->mode.limit=atoi(parv[args]);
				if(chptr->mode.limit<1)
				  chptr->mode.limit=1; /* clueless user, :) */
				*mbuf++='l';
				/* oops...we can't just shove the users args in, how obnoxious...*/
				ircsprintf(pbuf+len, "%d ", chptr->mode.limit);
				len=strlen(pbuf); /* yeah, who knows, heh! */
				args++;
				nmodes++;
				break;
			}
		 case 'k':
			if(level<1) {
				errors|=SM_ERR_NOPRIVS;
				break;
			}
			if(parv[args]==NULL)
			  break;
			/* if we're going to overflow our mode buffer,
			 * drop the change instead */
			i=strlen(parv[args]);
			if(len+i>MODEBUFLEN) {
				args++;
				break;
			}
			
			/* if they're an op, they can futz with the key in
			 * any manner they like, we're not picky */
			if(change=='+') {
				strncpy(chptr->mode.key,parv[args],KEYLEN);
				strcat(pbuf,parv[args]);
				strcat(pbuf," ");
				len+=i+1;
			}
			else {
				strcat(pbuf, chptr->mode.key);
				strcat(pbuf, " ");
				i=strlen(chptr->mode.key);
				len+=i+1;
				*chptr->mode.key=0;
			}
			*mbuf++='k';
			args++;
			nmodes++;
			break;
		 case 'r':
			if (!IsServer(sptr) && !IsULine(sptr)) {
				sendto_one(sptr, err_str(ERR_ONLYSERVERSCANCHANGE),
							  me.name, cptr->name, chptr->chname);
				break;
			}
			else {
				if(change=='+')
				  chptr->mode.mode|=MODE_REGISTERED;
				else
				  chptr->mode.mode&=~MODE_REGISTERED;
			}
			*mbuf++='r';
			nmodes++;
			break;
		 case 'i':
			if(level<1) {
				errors|=SM_ERR_NOPRIVS;
				break;
			}
			if(change=='-')
				while ((lp=chptr->invites))
				  del_invite(lp->value.cptr, chptr);
			/* fall through to default case */
		 default:
			/* phew, no more tough modes. }:>, the rest are all covered in one step 
			 * with the above array */
			if(level<1) {
				errors|=SM_ERR_NOPRIVS;
				break;
			}
			for(i=1;flags[i]!=0x0;i+=2) {
				if(*modes==(char)flags[i]) {
					if(change=='+')
					  chptr->mode.mode|=flags[i-1];
					else
					  chptr->mode.mode&=~flags[i-1];
					*mbuf++=*modes;
					nmodes++;
					break;
				}
			}
			/* unknown mode.. */
			if(flags[i]==0x0) {
				/* we still spew lots of unknown mode bits...*/
				/* but only to our own clients, silently ignore bogosity
				 * from other servers... */
				if(MyClient(sptr))
				  sendto_one(sptr, err_str(ERR_UNKNOWNMODE), me.name, sptr->name, *modes);
			}
			break;
		}
		/* spit out more parameters error here */
		if(errors&SM_ERR_MOREPARMS && MyClient(sptr)) {
			moreparmsstr[5]=change;
			moreparmsstr[6]=*modes;
			sendto_one(sptr, err_str(ERR_NEEDMOREPARAMS),
						  me.name, sptr->name, moreparmsstr);
			errors&=~SM_ERR_MOREPARMS; /* oops, kill it in this case */
		}
		modes++;
	}
	/* clean up the end of the string... */
	if(*(mbuf-1)=='+' || *(mbuf-1)=='-')
	  *(mbuf-1)=0;
	else
	  *mbuf=0;
	pbuf[len-1]=0;
	if(MyClient(sptr)) {
		if(errors&SM_ERR_NOPRIVS)
		  sendto_one(sptr, err_str(ERR_CHANOPRIVSNEEDED), me.name,
						 sptr->name, chptr->chname);	  
		if(errors&SM_ERR_RESTRICTED)
		  sendto_one(sptr,":%s NOTICE %s :*** Notice -- You are restricted and cannot chanop others",
						 me.name, sptr->name);
	}
	/* all done! */
	return nmodes;
}
				
static int
can_join(aClient *sptr, aChannel *chptr, char *key)
{
   Reg Link   *lp;
   int invited=0;
   for(lp=sptr->user->invited;lp;lp=lp->next) {
      if(lp->value.chptr==chptr) {
          invited=1;
          break;
      }
   }
	if (invited || IsULine(sptr))
		 return 0;
   if (is_banned(sptr, chptr))
	 return (ERR_BANNEDFROMCHAN);
   if (chptr->mode.mode & MODE_INVITEONLY)
         return (ERR_INVITEONLYCHAN);
   if (chptr->mode.mode & MODE_REGONLY && !IsRegNick(sptr))
         return (ERR_NEEDREGGEDNICK);
   if (*chptr->mode.key && (BadPtr(key) || mycmp(chptr->mode.key, key)))
      return (ERR_BADCHANNELKEY);
   if (chptr->mode.limit && chptr->users >= chptr->mode.limit) 
	 return (ERR_CHANNELISFULL);
   return 0;
}
/*
 * * Remove bells and commas from channel name
 */

void
clean_channelname(unsigned char *cn)
{
   for (; *cn; cn++)
      /*
       * All characters >33 are allowed, except commas, and the weird
		 * fake-space character mIRCers whine about -wd
       */
      if (*cn < 33 || *cn == ',' || (*cn == 160)) {
			*cn = '\0';
			return;
      }
	return;
}
/* we also tell the client if the channel is invalid. */
int check_channelname(aClient *cptr, unsigned char *cn) {
	if(!MyClient(cptr))
	  return 1;
	for(;*cn;cn++) {
		if(*cn<33 || *cn == ',' || *cn==160) {
			sendto_one(cptr, getreply(ERR_BADCHANNAME), me.name, cptr->name,
						  cn);
			return 0;
		}
	}
	return 1;
}

/*
 * *  Get Channel block for chname (and allocate a new channel *
 * block, if it didn't exist before).
 */
static aChannel *
get_channel(aClient *cptr,
	    char *chname,
	    int flag)
{
   Reg aChannel *chptr;
   int         len;

   if (BadPtr(chname))
      return NULL;

   len = strlen(chname);
   if (MyClient(cptr) && len > CHANNELLEN) {
      len = CHANNELLEN;
      *(chname + CHANNELLEN) = '\0';
   }
   if ((chptr = find_channel(chname, (aChannel *) NULL)))
      return (chptr);
   if (flag == CREATE) {
      chptr = (aChannel *) MyMalloc(sizeof(aChannel));
      memset((char *) chptr, '\0', sizeof(aChannel));

      strncpyzt(chptr->chname, chname, len + 1);
      if (channel)
		  channel->prevch = chptr;
      chptr->prevch = NULL;
      chptr->nextch = channel;
      channel = chptr;
		chptr->creationtime = chptr->channelts = timeofday;
      (void) add_to_channel_hash_table(chname, chptr);
      Count.chan++;
   }
   return chptr;
}

static void
add_invite(aClient *cptr, aChannel *chptr)
{
   Reg Link   *inv, **tmp;

   del_invite(cptr, chptr);
   /*
    * delete last link in chain if the list is max length
    */
   if (list_length(cptr->user->invited) >= MAXCHANNELSPERUSER) {
      /*
       * This forgets the channel side of invitation     -Vesa inv =
       * cptr->user->invited; cptr->user->invited = inv->next;
       * free_link(inv);
       */
      del_invite(cptr, cptr->user->invited->value.chptr);

   }
   /*
    * add client to channel invite list
    */
   inv = make_link();
   inv->value.cptr = cptr;
   inv->next = chptr->invites;
   chptr->invites = inv;
   /*
    * add channel to the end of the client invite list
    */
   for (tmp = &(cptr->user->invited); *tmp; tmp = &((*tmp)->next));
   inv = make_link();
   inv->value.chptr = chptr;
   inv->next = NULL;
   (*tmp) = inv;
}
/*
 * Delete Invite block from channel invite list and client invite list
 */
void
del_invite(aClient *cptr, aChannel *chptr)
{
   Reg Link  **inv, *tmp;

   for (inv = &(chptr->invites); (tmp = *inv); inv = &tmp->next)
      if (tmp->value.cptr == cptr) {
	 *inv = tmp->next;
	 free_link(tmp);
	 break;
      }

   for (inv = &(cptr->user->invited); (tmp = *inv); inv = &tmp->next)
      if (tmp->value.chptr == chptr) {
	 *inv = tmp->next;
	 free_link(tmp);
	 break;
      }
}
/*
 * *  Subtract one user from channel i (and free channel *  block, if
 * channel became empty).
 */
static void
sub1_from_channel(aChannel *chptr)
{
   Reg Link   *tmp;
   Link       *obtmp;

   if (--chptr->users <= 0) {
      /*
       * Now, find all invite links from channel structure
       */
      while ((tmp = chptr->invites))
	 del_invite(tmp->value.cptr, chptr);

      tmp = chptr->banlist;
      while (tmp) {
	 obtmp = tmp;
	 tmp = tmp->next;
	 MyFree(obtmp->value.banptr->banstr);
	 MyFree(obtmp->value.banptr->who);
	 MyFree(obtmp->value.banptr);
	 free_link(obtmp);
      }
      if (chptr->prevch)
	 chptr->prevch->nextch = chptr->nextch;
      else
	 channel = chptr->nextch;
      if (chptr->nextch)
	 chptr->nextch->prevch = chptr->prevch;
      (void) del_from_channel_hash_table(chptr->chname, chptr);
#ifdef FLUD
      free_fluders(NULL, chptr);
#endif
      MyFree((char *) chptr);
      Count.chan--;
   }
}

/*
 * * m_join * parv[0] = sender prefix *       parv[1] = channel *
 * parv[2] = channel password (key)
 */
int
m_join(aClient *cptr,
       aClient *sptr,
       int parc,
       char *parv[])
{
   static char jbuf[BUFSIZE];
   Reg Link   *lp;
   Reg aChannel *chptr;
   Reg char   *name, *key = NULL;
   int         i, flags = 0, chanlen=0;
	
#ifdef USE_ALLOW_OP
   int         allow_op = YES;
	
#endif
   char       *p = NULL, *p2 = NULL;
	
#ifdef ANTI_SPAMBOT
   int         successful_join_count = 0;	
	/* Number of channels successfully joined */
#endif
	
   if (!(sptr->user)) {
      /* something is *fucked* - bail */
      return 0;
   }
	
   if (parc < 2 || *parv[1] == '\0') {
      sendto_one(sptr, err_str(ERR_NEEDMOREPARAMS),
					  me.name, parv[0], "JOIN");
      return 0;
   }
	
   *jbuf = '\0';
   /*
    * * Rebuild list of channels joined to be the actual result of the *
    * JOIN.  Note that "JOIN 0" is the destructive problem.
    */
   for (i = 0, name = strtoken(&p, parv[1], ","); name;
		  name = strtoken(&p, (char *) NULL, ",")) {
      /*
       * pathological case only on longest channel name. * If not dealt
       * with here, causes desynced channel ops * since ChannelExists()
       * doesn't see the same channel * as one being joined. cute bug.
       * Oct 11 1997, Dianora/comstud
       */
		if(!check_channelname(sptr, (unsigned char *) name))
		  continue;
		clean_channelname((unsigned char *)name);
		chanlen=strlen(name);
      if (chanlen > CHANNELLEN)	{ /* same thing is done in get_channel() */
			name[CHANNELLEN] = '\0';
			chanlen=CHANNELLEN;
		}
      if (*name == '&' && !MyConnect(sptr))
		  continue;
      if (*name == '0' && !atoi(name))
		  *jbuf = '\0';
      else if (!IsChannelName(name)) {
			if (MyClient(sptr))
			  sendto_one(sptr, err_str(ERR_NOSUCHCHANNEL),
							 me.name, parv[0], name);
			continue;
      }
      if (*jbuf)
		  (void) strcat(jbuf, ",");
      (void) strncat(jbuf, name, sizeof(jbuf) - i - 1);
      i += chanlen + 1;
   }
   /*
    * (void)strcpy(parv[1], jbuf); 
    */
	
   p = NULL;
   if (parv[2])
	  key = strtoken(&p2, parv[2], ",");
   parv[2] = NULL;		/*
								 * for m_names call later, parv[parc]
								 * * must == NULL 
								 */
   for (name = strtoken(&p, jbuf, ","); name;
		  key = (key) ? strtoken(&p2, NULL, ",") : NULL,
		  name = strtoken(&p, NULL, ",")) {
      /*
       * * JOIN 0 sends out a part for all channels a user * has
       * joined.
       */
      if (*name == '0' && !atoi(name)) {
			if (sptr->user->channel == NULL)
			  continue;
			while ((lp = sptr->user->channel)) {
				chptr = lp->value.chptr;
				sendto_channel_butserv(chptr, sptr, PartFmt,
											  parv[0], chptr->chname);
				remove_user_from_channel(sptr, chptr);
			}
			/*
			 * Added /quote set for SPAMBOT
			 * 
			 * int spam_time = MIN_JOIN_LEAVE_TIME; int spam_num =
			 * MAX_JOIN_LEAVE_COUNT;
			 */
#ifdef ANTI_SPAMBOT		/*
			* Dianora 
								 */
			
			if (MyConnect(sptr) && !IsAnOper(sptr)) {
				if (sptr->join_leave_count >= spam_num) {
					sendto_ops_lev(FLOOD_LEV, "User %s (%s@%s) is a possible spambot",
										sptr->name,
										sptr->user->username, sptr->user->host);
					sptr->oper_warn_count_down = OPER_SPAM_COUNTDOWN;
				}
				else {
					int         t_delta;
					
					if ((t_delta = (NOW - sptr->last_leave_time)) >
						 JOIN_LEAVE_COUNT_EXPIRE_TIME) {
						int         decrement_count;
						
						decrement_count = (t_delta / JOIN_LEAVE_COUNT_EXPIRE_TIME);
						
						if (decrement_count > sptr->join_leave_count)
						  sptr->join_leave_count = 0;
						else
						  sptr->join_leave_count -= decrement_count;
					}
					else {
						if ((NOW - (sptr->last_join_time)) < spam_time) {
							/*
							 * oh, its a possible spambot 
							 */
							sptr->join_leave_count++;
						}
					}
					sptr->last_leave_time = NOW;
				}
			}
#endif
			sendto_match_servs(NULL, cptr, ":%s JOIN 0", parv[0]);
			continue;
      }
		
      chptr = get_channel(sptr, name, CREATE);

      if (MyConnect(sptr)) {
			/*
			 * * local client is first to enter previously nonexistent *
			 * channel so make them (rightfully) the Channel * Operator.
			 */
			flags = (ChannelExists(name)) ? 0 : CHFL_CHANOP;
#ifdef NO_CHANOPS_WHEN_SPLIT
			if (!IsAnOper(sptr) && server_was_split && server_split_recovery_time) {
				if ((server_split_time + server_split_recovery_time) < NOW) {
					if (serv_fdlist.entry[1] > serv_fdlist.last_entry)
					  server_was_split = NO;
					else {
						server_split_time = NOW;	/*
															 * still split 
															 */
						allow_op = NO;
					}
				}
				else {
					allow_op = NO;
				}
				if (!IsRestricted(sptr) && !allow_op && can_join(sptr, chptr, key))
				  sendto_one(sptr, ":%s NOTICE %s :*** Notice -- Due to a network split, you can not obtain channel operator status in a new channel at this time.",
								 me.name,
								 sptr->name);
			}
#endif
			
#ifdef LITTLE_I_LINES
			if (!IsAnOper(sptr) && IsRestricted(sptr)) {
				allow_op = NO;
				sendto_one(sptr, ":%s NOTICE %s :*** Notice -- You are restricted and cannot be chanopped",
							  me.name,
							  sptr->name);
			}
#endif
			if ((sptr->user->joined >= MAXCHANNELSPERUSER) &&
				 (!IsAnOper(sptr) || (sptr->user->joined >= MAXCHANNELSPERUSER * 3))) {
				sendto_one(sptr, err_str(ERR_TOOMANYCHANNELS),
							  me.name, parv[0], name);
#ifdef ANTI_SPAMBOT
				if (successful_join_count)
				  sptr->last_join_time = NOW;
#endif
				return 0;
			}
#ifdef ANTI_SPAMBOT		/*
			* Dianora 
								 */
			if (flags == 0)	/*
									 * if channel doesn't exist, don't
									 * * penalize 
									 */
			  successful_join_count++;
			if (sptr->join_leave_count >= spam_num) {
				/*
				 * Its already known as a possible spambot 
				 */
				
				if (sptr->oper_warn_count_down > 0)		/*
																	 * my general paranoia 
																	 */
				  sptr->oper_warn_count_down--;
				else
				  sptr->oper_warn_count_down = 0;
				
				if (sptr->oper_warn_count_down == 0) {
					sendto_ops_lev(FLOOD_LEV, "User %s (%s@%s) trying to join %s is a possible spambot",
										sptr->name,
										sptr->user->username,
										sptr->user->host,
										name);
					sptr->oper_warn_count_down = OPER_SPAM_COUNTDOWN;
				}
# ifndef ANTI_SPAMBOT_WARN_ONLY
				return 0;		/*
									 * Don't actually JOIN anything, but
									 * * don't let spambot know that 
									 */
# endif
			}
#endif
      }
      else {
			/*
			 * * complain for remote JOINs to existing channels * (they
			 * should be SJOINs) -orabidoo
			 */
				if (!ChannelExists(name))
				  ts_warn("User on %s remotely JOINing new channel", sptr->user->server);
      }
			
		
      if (!chptr ||
			 (MyConnect(sptr) && (i = can_join(sptr, chptr, key)))) {
			sendto_one(sptr,
						  ":%s %d %s %s :Sorry, cannot join channel.",
						  me.name, i, parv[0], name);
#ifdef ANTI_SPAMBOT
			if (successful_join_count > 0)
			  successful_join_count--;
#endif
			continue;
      }
      if (IsMember(sptr, chptr))
		  continue;
      /*
       * *  Complete user entry to the new channel (if any)
       */
#ifdef USE_ALLOW_OP
      if (allow_op)
		  add_user_to_channel(chptr, sptr, flags);
      else
		  add_user_to_channel(chptr, sptr, 0);
#else
      add_user_to_channel(chptr, sptr, flags);
#endif
      /*
       * *  Set timestamp if appropriate, and propagate
       */
      if (MyClient(sptr) && flags == CHFL_CHANOP) {
			chptr->channelts = timeofday;
			
#ifdef USE_ALLOW_OP
			if (allow_op)
			sendto_match_servs(chptr, cptr, ":%s SJOIN %ld %ld %s + :@%s",
									 me.name, chptr->channelts, chptr->creationtime, name, parv[0]);
			else
			sendto_match_servs(chptr, cptr,
									 ":%s SJOIN %ld %ld %s + :%s", me.name,
									 chptr->channelts, chptr->creationtime, name, parv[0]);
#else
			sendto_match_servs(chptr, cptr, ":%s SJOIN %ld %ld %s + :@%s",
									 me.name, chptr->channelts, chptr->creationtime, name, parv[0]);
#endif
      }
      else if (MyClient(sptr)) {
			sendto_match_servs(chptr, cptr,
									 ":%s SJOIN %ld %ld %s + :%s", me.name,
									 chptr->channelts, chptr->creationtime, name, parv[0]);
      }
      else {
			sendto_match_servs(chptr, cptr, ":%s JOIN :%s", parv[0],
									 name);
		}
		/*
       * * notify all other users on the new channel
       */
      sendto_channel_butserv(chptr, sptr, ":%s JOIN :%s",
									  parv[0], name);
		
      if (MyClient(sptr)) {
			del_invite(sptr, chptr);
			if (chptr->topic[0] != '\0') {
				sendto_one(sptr, rpl_str(RPL_TOPIC), me.name,
							  parv[0], name, chptr->topic);
				sendto_one(sptr, rpl_str(RPL_TOPICWHOTIME),
							  me.name, parv[0], name,
							  chptr->topic_nick,
							  chptr->topic_time);
			}
			parv[1] = name;
			(void) m_names(cptr, sptr, 2, parv);
      }
   }
	
#ifdef ANTI_SPAMBOT
   if (MyConnect(sptr) && successful_join_count)
	  sptr->last_join_time = NOW;
#endif
   return 0;
}
/*
 * * m_part * parv[0] = sender prefix *       parv[1] = channel *
 * parv[2] = Optional part reason
 */
int
m_part(aClient *cptr,
       aClient *sptr,
       int parc,
       char *parv[])
{
   Reg aChannel *chptr;
   char       *p, *name;
   register char *reason = (parc > 2 && parv[2]) ? parv[2] : NULL;

   if (parc < 2 || parv[1][0] == '\0') {
      sendto_one(sptr, err_str(ERR_NEEDMOREPARAMS),
		 me.name, parv[0], "PART");
      return 0;
   }

   name = strtoken(&p, parv[1], ",");

#ifdef ANTI_SPAMBOT		/*
				 * Dianora 
				 */
   /*
    * if its my client, and isn't an oper 
    */

   if (name && MyConnect(sptr) && !IsAnOper(sptr)) {
      if (sptr->join_leave_count >= spam_num) {
	 sendto_realops("User %s (%s@%s) is a possible spambot",
			sptr->name,
			sptr->user->username, sptr->user->host);
	 sptr->oper_warn_count_down = OPER_SPAM_COUNTDOWN;
      }
      else {
   int         t_delta;

	 if ((t_delta = (NOW - sptr->last_leave_time)) >
	     JOIN_LEAVE_COUNT_EXPIRE_TIME) {
   int         decrement_count;

	    decrement_count = (t_delta / JOIN_LEAVE_COUNT_EXPIRE_TIME);

	    if (decrement_count > sptr->join_leave_count)
	       sptr->join_leave_count = 0;
	    else
	       sptr->join_leave_count -= decrement_count;
	 }
	 else {
	    if ((NOW - (sptr->last_join_time)) < spam_time) {
	       /*
	        * oh, its a possible spambot 
	        */
	       sptr->join_leave_count++;
	    }
	 }
	 sptr->last_leave_time = NOW;
      }
   }
#endif

   while (name) {
      chptr = get_channel(sptr, name, 0);
      if (!chptr) {
	 sendto_one(sptr, err_str(ERR_NOSUCHCHANNEL),
		    me.name, parv[0], name);
	 name = strtoken(&p, (char *) NULL, ",");
	 continue;
      }

      if (!IsMember(sptr, chptr)) {
	 sendto_one(sptr, err_str(ERR_NOTONCHANNEL),
		    me.name, parv[0], name);
	 name = strtoken(&p, (char *) NULL, ",");
	 continue;
      }
      /*
       * *  Remove user from the old channel (if any)
       */

      if (parc < 3)
	 sendto_match_servs(chptr, cptr, PartFmt, parv[0], name);
      else
	 sendto_match_servs(chptr, cptr, PartFmt2, parv[0], name, reason);
      if (parc < 3)
	 sendto_channel_butserv(chptr, sptr, PartFmt, parv[0], name);
      else
	 sendto_channel_butserv(chptr, sptr, PartFmt2, parv[0], name, reason);
      remove_user_from_channel(sptr, chptr);
      name = strtoken(&p, (char *) NULL, ",");
   }
   return 0;
}
/*
 * * m_kick * parv[0] = sender prefix *       parv[1] = channel *
 * parv[2] = client to kick *   parv[3] = kick comment
 */
int
m_kick(aClient *cptr,
       aClient *sptr,
       int parc,
       char *parv[])
{
   aClient    *who;
   aChannel   *chptr;
   int         chasing = 0;
   int         user_count;	/*

				 * count nicks being kicked, only allow 4 
				 */
   char       *comment, *name, *p = NULL, *user, *p2 = NULL;

   if (parc < 3 || *parv[1] == '\0') {
      sendto_one(sptr, err_str(ERR_NEEDMOREPARAMS),
		 me.name, parv[0], "KICK");
      return 0;
   }
   if (IsServer(sptr) && !IsULine(sptr))
      sendto_ops("KICK from %s for %s %s",
		 parv[0], parv[1], parv[2]);
   comment = (BadPtr(parv[3])) ? parv[0] : parv[3];
   if (strlen(comment) > (size_t) TOPICLEN)
      comment[TOPICLEN] = '\0';

   *nickbuf = *buf = '\0';
   name = strtoken(&p, parv[1], ",");

   while (name) {
      chptr = get_channel(sptr, name, !CREATE);
      if (!chptr) {
	 sendto_one(sptr, err_str(ERR_NOSUCHCHANNEL),
		    me.name, parv[0], name);
	 name = strtoken(&p, (char *) NULL, ",");
	 continue;
      }

      /*
       * You either have chan op privs, or you don't -Dianora 
       */
      /*
       * orabidoo and I discussed this one for a while... I hope he
       * approves of this code, users can get quite confused...
       * -Dianora
       */

      if (!IsServer(sptr) && !is_chan_op(sptr, chptr) && !IsULine(sptr)) {
	 /*
	  * was a user, not a server, and user isn't seen as a chanop
	  * here
	  */

	 if (MyConnect(sptr)) {
	    /*
	     * user on _my_ server, with no chanops.. so go away 
	     */

	    sendto_one(sptr, err_str(ERR_CHANOPRIVSNEEDED),
		       me.name, parv[0], chptr->chname);
	    name = strtoken(&p, (char *) NULL, ",");
	    continue;
	 }

	 if (chptr->channelts == 0) {
	    /*
	     * If its a TS 0 channel, do it the old way 
	     */

	    sendto_one(sptr, err_str(ERR_CHANOPRIVSNEEDED),
		       me.name, parv[0], chptr->chname);
	    name = strtoken(&p, (char *) NULL, ",");
	    continue;
	 }
	 /*
	  * Its a user doing a kick, but is not showing as chanop
	  * locally its also not a user ON -my- server, and the channel
	  * has a TS. There are two cases we can get to this point
	  * then...
	  * 
	  * 1) connect burst is happening, and for some reason a legit op
	  * has sent a KICK, but the SJOIN hasn't happened yet or been
	  * seen. (who knows.. due to lag...)
	  * 
	  * 2) The channel is desynced. That can STILL happen with TS
	  * 
	  * Now, the old code roger wrote, would allow the KICK to go
	  * through. Thats quite legit, but lets weird things like
	  * KICKS by users who appear not to be chanopped happen, or
	  * even neater, they appear not to be on the channel. This
	  * fits every definition of a desync, doesn't it? ;-) So I
	  * will allow the KICK, otherwise, things are MUCH worse. But
	  * I will warn it as a possible desync.
	  * 
	  * -Dianora
	  */
	 /*
	  * sendto_one(sptr, err_str(ERR_DESYNC), me.name, parv[0],
	  * chptr->chname);
	  */
	 /*
	  * After more discussion with orabidoo...
	  * 
	  * The code was sound, however, what happens if we have +h (TS4)
	  * and some servers don't understand it yet? we will be seeing
	  * servers with users who appear to have no chanops at all,
	  * merrily kicking users.... -Dianora
	  * 
	  */
      }

      user = strtoken(&p2, parv[2], ",");
      user_count = 4;
      while (user && user_count) {
	 user_count--;
	 if (!(who = find_chasing(sptr, user, &chasing))) {
	    user = strtoken(&p2, (char *) NULL, ",");
	    continue;		/*
				 * No such user left! 
				 */
	 }

	 if (IsMember(who, chptr)) {
	    sendto_channel_butserv(chptr, sptr,
				   ":%s KICK %s %s :%s", parv[0],
				   name, who->name, comment);
	    sendto_match_servs(chptr, cptr,
			       ":%s KICK %s %s :%s",
			       parv[0], name,
			       who->name, comment);
	    remove_user_from_channel(who, chptr);
	 }
	 else
	    sendto_one(sptr, err_str(ERR_USERNOTINCHANNEL),
		       me.name, parv[0], user, name);
	 user = strtoken(&p2, (char *) NULL, ",");
      }				/*
				 * loop on parv[2] 
				 */

      name = strtoken(&p, (char *) NULL, ",");
   }				/*
				 * loop on parv[1] 
				 */

   return (0);
}

int
count_channels(aClient *sptr)
{
   Reg aChannel *chptr;
   Reg int     count = 0;

   for (chptr = channel; chptr; chptr = chptr->nextch)
      count++;
   return (count);
}
/*
 * * m_topic *        parv[0] = sender prefix *       parv[1] = topic text
 */
int
m_topic(aClient *cptr,
		  aClient *sptr,
		  int parc,
		  char *parv[])
{
   aChannel   *chptr = NullChn;
   char       *topic = NULL, *name, *tnick=sptr->name;
	time_t ts=timeofday;
	
   if (parc < 2) {
      sendto_one(sptr, err_str(ERR_NEEDMOREPARAMS),
					  me.name, parv[0], "TOPIC");
      return 0;
   }
	
   name = parv[1];
	chptr = find_channel(name, NullChn);
	if(!chptr) {
		sendto_one(sptr, err_str(ERR_NOSUCHCHANNEL), me.name, parv[0], name);
		return 0;
	}
	
	if (parc > 1 && IsChannelName(name)) {
		if(!IsMember(sptr, chptr) && !IsServer(sptr) && !IsULine(sptr)) {
			sendto_one(sptr, err_str(ERR_NOTONCHANNEL),
						  me.name, parv[0], name);
			return 0;
		}
		 if (parc>3 && (!MyConnect(sptr) || IsULine(sptr) || IsServer(sptr))) {
				topic=(parc>4 ? parv[4] : "");
				tnick=parv[2];
				ts=atoi(parv[3]);
		 } else {
				topic=parv[2];
		 }
	}
	
	if (!topic) {		/*
							 * only asking  for topic  
							 */
		if (chptr->topic[0] == '\0')
		  sendto_one(sptr, rpl_str(RPL_NOTOPIC),
						 me.name, parv[0], chptr->chname);
		else {
			sendto_one(sptr, rpl_str(RPL_TOPIC),
						  me.name, parv[0],
						  chptr->chname, chptr->topic);
			sendto_one(sptr, rpl_str(RPL_TOPICWHOTIME),
						  me.name, parv[0], chptr->chname,
						  chptr->topic_nick,
						  chptr->topic_time);
		}
	}
	else if (((!(chptr->mode.mode & MODE_TOPICLIMIT) || is_chan_op(sptr, chptr))
				|| IsULine(sptr) || IsServer(sptr)) && topic) {
		/*
		 * setting a topic 
		 */
		strncpyzt(chptr->topic, topic, sizeof(chptr->topic));
		strcpy(chptr->topic_nick, tnick);
		chptr->topic_time = ts;
		
		/* in this case I think it's better that we send all the info that df 
		 * sends with the topic, so I changed everything to work like that.
		 * Yes, this means we can't link to full hybrid servers, oops, :) 
		 * -wd */
		sendto_match_servs(chptr, cptr, ":%s TOPIC %s %s %lu :%s",
								 parv[0], chptr->chname, chptr->topic_nick,
								 chptr->topic_time, chptr->topic);
		sendto_channel_butserv(chptr, sptr, ":%s TOPIC %s :%s",
									  parv[0],
									  chptr->chname, chptr->topic);
	}
	else
	  sendto_one(sptr, err_str(ERR_CHANOPRIVSNEEDED),
					 me.name, parv[0], chptr->chname);
	
	
   return 0;
}
/*
 * * m_invite *       parv[0] - sender prefix *       parv[1] - user to
 * invite *     parv[2] - channel number
 */
int
m_invite(aClient *cptr,
	 aClient *sptr,
	 int parc,
	 char *parv[])
{
   aClient    *acptr;
   aChannel   *chptr;

   if (parc < 3 || *parv[1] == '\0') {
      sendto_one(sptr, err_str(ERR_NEEDMOREPARAMS),
		 me.name, parv[0], "INVITE");
      return -1;
   }

   if (!(acptr = find_person(parv[1], (aClient *) NULL))) {
      sendto_one(sptr, err_str(ERR_NOSUCHNICK),
		 me.name, parv[0], parv[1]);
      return 0;
   }
	if(!check_channelname(sptr, (unsigned char *)parv[2]))
	  return 0;
   clean_channelname((unsigned char *) parv[2]);

   if (!(chptr = find_channel(parv[2], NullChn))) {
      sendto_prefix_one(acptr, sptr, ":%s INVITE %s :%s",
			parv[0], parv[1], parv[2]);
      return 0;
   }

   if (chptr && !IsMember(sptr, chptr) && !IsULine(sptr)) {
      sendto_one(sptr, err_str(ERR_NOTONCHANNEL),
		 me.name, parv[0], parv[2]);
      return -1;
   }

   if (IsMember(acptr, chptr)) {
      sendto_one(sptr, err_str(ERR_USERONCHANNEL),
		 me.name, parv[0], parv[1], parv[2]);
      return 0;
   }
   if (chptr && (chptr->mode.mode & MODE_INVITEONLY)) {
      if (!is_chan_op(sptr, chptr) && (!IsULine(sptr))) {
	 sendto_one(sptr, err_str(ERR_CHANOPRIVSNEEDED),
		    me.name, parv[0], chptr->chname);
	 return -1;
      }
      else if (!IsMember(sptr, chptr) && !IsULine(sptr)) {
	 sendto_one(sptr, err_str(ERR_CHANOPRIVSNEEDED),
		    me.name, parv[0],
		    ((chptr) ? (chptr->chname) : parv[2]));
	 return -1;
      }
   }

   if (MyConnect(sptr)) {
      sendto_one(sptr, rpl_str(RPL_INVITING), me.name, parv[0],
		 acptr->name, ((chptr) ? (chptr->chname) : parv[2]));
      if (acptr->user->away)
	 sendto_one(sptr, rpl_str(RPL_AWAY), me.name, parv[0],
		    acptr->name, acptr->user->away);
   }
   if (MyConnect(acptr))
      if ((chptr && (chptr->mode.mode & MODE_INVITEONLY) &&
			  sptr->user && is_chan_op(sptr, chptr)) || IsULine(sptr)) {
			add_invite(acptr, chptr);
			sendto_channelops_butone(NULL, &me, chptr, ":%s NOTICE @%s :%s invited %s into channel %s.",
											 me.name, chptr->chname, sptr->name, acptr->name, chptr->chname);
		}
   sendto_prefix_one(acptr, sptr, ":%s INVITE %s :%s", parv[0],
		  acptr->name, ((chptr) ? (chptr->chname) : parv[2]));
   return 0;
}


/*
 * The function which sends the actual channel list back to the user.
 * Operates by stepping through the hashtable, sending the entries back if
 * they match the criteria.
 * cptr = Local client to send the output back to.
 * numsend = Number (roughly) of lines to send back. Once this number has
 * been exceeded, send_list will finish with the current hash bucket,
 * and record that number as the number to start next time send_list
 * is called for this user. So, this function will almost always send
 * back more lines than specified by numsend (though not by much,
 * assuming CH_MAX is was well picked). So be conservative in your choice
 * of numsend. -Rak
 */

void
send_list(aClient *cptr,
	int numsend)
{
    aChannel	*chptr;
    LOpts	*lopt = cptr->user->lopt;
    int		hashnum;

    for (hashnum = lopt->starthash; hashnum < CH_MAX; hashnum++)
    {
	if (numsend > 0)
	    for (chptr = (aChannel *)hash_get_chan_bucket(hashnum); 
			chptr; chptr = chptr->hnextch)
	    {
		if (SecretChannel(chptr) && !IsMember(cptr, chptr))
		    continue;
		if ((!lopt->showall) && ((chptr->users < lopt->usermin) ||
			((lopt->usermax >= 0) && (chptr->users > lopt->usermax)) ||
			((chptr->creationtime||1) < lopt->chantimemin) ||
			(chptr->topic_time < lopt->topictimemin) ||
			(chptr->creationtime > lopt->chantimemax) ||
			(chptr->topic_time > lopt->topictimemax) ||
			(lopt->nolist && 
				find_str_link(lopt->nolist, chptr->chname)) ||
			(lopt->yeslist && 
				!find_str_link(lopt->yeslist, chptr->chname))))
					continue;
		sendto_one(cptr, rpl_str(RPL_LIST), me.name, cptr->name,
			ShowChannel(cptr, chptr) ? chptr->chname : "*",
			chptr->users,
			ShowChannel(cptr, chptr) ? chptr->topic : "");
		numsend--;
	    }
	else
	    break;
    }

    /* All done */
    if (hashnum == CH_MAX)
    {
	Link *lp, *next;
	sendto_one(cptr, rpl_str(RPL_LISTEND), me.name, cptr->name);
	for (lp = lopt->yeslist; lp; lp = next)
	{
	    next = lp->next;
	    free_link(lp);
	}
	for (lp = lopt->nolist; lp; lp = next)
	{
	    next = lp->next;
	    free_link(lp);
	}

	MyFree(cptr->user->lopt);
	cptr->user->lopt = NULL;
	return;
    }

    /* 
     * We've exceeded the limit on the number of channels to send back
     * at once.
     */
    lopt->starthash = hashnum;
    return;
}


/*
 * * m_list *      parv[0] = sender prefix *      parv[1] = channel
 */
int
m_list(aClient *cptr,
       aClient *sptr,
       int parc,
       char *parv[])
{
    aChannel	*chptr;
    time_t	currenttime = time(NULL);
    char	*name, *p = NULL;
    LOpts	*lopt = NULL;
    Link	*lp, *next;
    int		usermax, usermin, error = 0, doall = 0;
    time_t	chantimemin, chantimemax;
    ts_val	topictimemin, topictimemax;
    Link 	*yeslist = NULL, *nolist = NULL;

    static char *usage[] = {
	"   Usage: /raw LIST options (on mirc) or /quote LIST options (ircII)",
	"",
	"If you don't include any options, the default is to send you the",
	"entire unfiltered list of channels. Below are the options you can",
	"use, and what channels LIST will return when you use them.",
	">number  List channels with more than <number> people.",
	"<number  List channels with less than <number> people.",
	"C>number List channels created between now and <number> minutes ago.",
	"C<number List channels created earlier than <number> minutes ago.",
	"T>number List channels whose topics are older than <number> minutes",
	"         (Ie, they have not changed in the last <number> minutes.",
	"T<number List channels whose topics are not older than <number> minutes.",
	"*mask*   List channels that match *mask*",
	"!*mask*  List channels that do not match *mask*",
	NULL
    };

    /* Some starting san checks -- No interserver lists allowed. */
    if (cptr != sptr || !sptr->user) return 0;

    /* If a /list is in progress, then another one will cancel it */
    if ((lopt = sptr->user->lopt)!=NULL)
    {
	sendto_one(sptr, rpl_str(RPL_LISTEND), me.name, parv[0]);
	for (lp = lopt->yeslist; lp; lp = next)
	{
	    next = lp->next;
	    free_link(lp);
	}
	for (lp = lopt->nolist; lp; lp = next)
	{
	    next = lp->next;
	    free_link(lp);
	}
	MyFree(sptr->user->lopt);
	sptr->user->lopt = NULL;
	return 0;
    }

	/* if HTM, drop this too */
	if(lifesux) {
		sendto_one(sptr, rpl_str(RPL_LOAD2HI), me.name, sptr->name);
		return 0;
	}
		
	if (parc < 2 || BadPtr(parv[1])) {

	sendto_one(sptr, rpl_str(RPL_LISTSTART), me.name, parv[0]);
	lopt = sptr->user->lopt = (LOpts *) MyMalloc(sizeof(LOpts));
	memset(lopt, '\0', sizeof(LOpts));

	lopt->showall = 1;

	if (DBufLength(&cptr->sendQ) < 2048)
	    send_list(cptr, 64);

        return 0;
   }

   if ((parc == 2) && (parv[1][0] == '?') && (parv[1][1] == '\0'))
   {
	char **ptr = usage;
	for (; *ptr; ptr++)
	    sendto_one(sptr, rpl_str(RPL_COMMANDSYNTAX), me.name,
		   cptr->name, *ptr);
	return 0;
   }

   sendto_one(sptr, rpl_str(RPL_LISTSTART), me.name, parv[0]);

   chantimemax = topictimemax = currenttime + 86400;
   chantimemin = topictimemin = 0;
   usermin = 2; /* By default, set the minimum to 2 users */
   usermax = -1; /* No maximum */

   for (name = strtoken(&p, parv[1], ","); name && !error;
		name = strtoken(&p, (char *) NULL, ","))
   {

      switch (*name)
      {
	  case '<':
	     usermax = atoi(name+1) - 1;
	     doall = 1;
	     break;
	  case '>':
	     usermin = atoi(name+1) + 1;
	     doall = 1;
	     break;
	  case 'C':
	  case 'c': /* Channel TS time -- creation time? */
	     ++name;
	     switch (*name++)
	     {
		case '<':
		   chantimemax = currenttime - 60 * atoi(name);
		   doall = 1;
		   break;
		case '>':
		   chantimemin = currenttime - 60 * atoi(name);
		   doall = 1;
		   break;
		default:
		   sendto_one(sptr, err_str(ERR_LISTSYNTAX), me.name, 
			cptr->name);
		   error = 1;
	     }
	     break;
	  case 'T':
	  case 't':
	     ++name;
	     switch (*name++)
	     {
		case '<':
		   topictimemax = currenttime - 60 * atoi(name);
		   doall = 1;
		   break;
		case '>':
		   topictimemin = currenttime - 60 * atoi(name);
		   doall = 1;
		   break;
		default:
		   sendto_one(sptr, err_str(ERR_LISTSYNTAX), me.name, 
			cptr->name);
		   error = 1;
	     }
	     break;
	  default: /* A channel, possibly with wildcards.
		    * Thought for the future: Consider turning wildcard
		    * processing on the fly.
		    * new syntax: !channelmask will tell ircd to ignore
		    * any channels matching that mask, and then
		    * channelmask will tell ircd to send us a list of
		    * channels only masking channelmask. Note: Specifying
		    * a channel without wildcards will return that
		    * channel even if any of the !channelmask masks
		    * matches it.
		    */
	     if (*name == '!')
	     {
		doall = 1;
		lp = make_link();
		lp->next = nolist;
		nolist = lp;
		DupString(lp->value.cp, name+1);
	     }
	     else if (strchr(name, '*') || strchr(name, '*'))
	     {
		doall = 1;
		lp = make_link();
		lp->next = yeslist;
		yeslist = lp;
		DupString(lp->value.cp, name);
	     }
	     else /* Just a normal channel */
	     {
		chptr = find_channel(name, NullChn);
		if (chptr && ShowChannel(sptr, chptr))
		   sendto_one(sptr, rpl_str(RPL_LIST), me.name, parv[0],
			ShowChannel(sptr, chptr) ? name : "*",
			chptr->users, chptr->topic);
	     }
	 } /* switch */
   } /* while */

   if (doall)
   {
      lopt = sptr->user->lopt = (LOpts *) MyMalloc(sizeof(LOpts));
      memset(lopt, '\0', sizeof(LOpts));
      lopt->usermin = usermin;
      lopt->usermax = usermax;
      lopt->topictimemax = topictimemax;
      lopt->topictimemin = topictimemin;
      lopt->chantimemax = chantimemax;
      lopt->chantimemin = chantimemin;
      lopt->nolist = nolist;
      lopt->yeslist = yeslist;

      if (DBufLength(&cptr->sendQ) < 2048)
	 send_list(cptr, 64);
      return 0;
   }

   sendto_one(sptr, rpl_str(RPL_LISTEND), me.name, parv[0]);

   return 0;
}



/************************************************************************
 * m_names() - Added by Jto 27 Apr 1989
 ************************************************************************/
/*
 * * m_names *        parv[0] = sender prefix *       parv[1] = channel
 */
/* maximum names para to show to opers when abuse occurs */
#define TRUNCATED_NAMES 20
int
m_names(aClient *cptr,
	aClient *sptr,
	int parc,
	char *parv[])
{
   Reg aChannel *chptr;
   Reg aClient *c2ptr;
   Reg Link   *lp;
   aChannel   *ch2ptr = NULL;
   int         idx, flag, len, mlen;
   char       *s, *para = parc > 1 ? parv[1] : NULL;
	 int char_count=0;
	 
	 
	 if (!MyConnect(sptr)) {
			sendto_one(sptr, rpl_str(RPL_ENDOFNAMES), me.name, parv[0], "*");
			return 0;
	 }
   mlen = strlen(me.name) + NICKLEN + 7;

   if (!BadPtr(para)) {
/* Here is the lamer detection code
 * P.S. meta, GROW UP
 * -Dianora
 */
		for(s = para; *s; s++) {
			char_count++;
			if(*s == ',') {
				if(char_count > TRUNCATED_NAMES)
				  para[TRUNCATED_NAMES] = '\0';
				else {
					s++;
					*s = '\0';
				}
				sendto_realops("/names abuser %s [%s]",
									para, get_client_name(sptr,FALSE));
				sendto_one(sptr, err_str(ERR_TOOMANYTARGETS),
							  me.name, sptr->name, "NAMES");
				return 0;
			}
		}
		s = strchr(para, ',');
		if (s)
		  *s = '\0';
		if(!check_channelname(sptr, (unsigned char *)para))
		  return 0;
		clean_channelname((unsigned char *) para);
		ch2ptr = find_channel(para, (aChannel *) NULL);
	}

   *buf = '\0';
   /*
    * Allow NAMES without registering
    * 
    * First, do all visible channels (public and the one user self is)
    */

   for (chptr = channel; chptr; chptr = chptr->nextch) {
      if ((chptr != ch2ptr) && !BadPtr(para))
	 continue;		/*
				 * -- wanted a specific channel 
				 */
      if (!MyConnect(sptr) && BadPtr(para))
	 continue;
      if (!ShowChannel(sptr, chptr))
	 continue;		/*
				 * -- users on this are not listed 
				 */

      /*
       * Find users on same channel (defined by chptr) 
       */

      (void) strcpy(buf, "* ");
      len = strlen(chptr->chname);
      (void) strcpy(buf + 2, chptr->chname);
      (void) strcpy(buf + 2 + len, " :");

      if (PubChannel(chptr))
	 *buf = '=';
      else if (SecretChannel(chptr))
	 *buf = '@';
      idx = len + 4;
      flag = 1;
      for (lp = chptr->members; lp; lp = lp->next) {
	 c2ptr = lp->value.cptr;
	 if (IsInvisible(c2ptr) && !IsMember(sptr, chptr))
	    continue;
	 if (lp->flags & CHFL_CHANOP) {
	    (void) strcat(buf, "@");
	    idx++;
	 }
	 else if (lp->flags & CHFL_VOICE) {
	    (void) strcat(buf, "+");
	    idx++;
	 }
	 (void) strncat(buf, c2ptr->name, NICKLEN);
	 idx += strlen(c2ptr->name) + 1;
	 flag = 1;
	 (void) strcat(buf, " ");
	 if (mlen + idx + NICKLEN > BUFSIZE - 3) {
	    sendto_one(sptr, rpl_str(RPL_NAMREPLY),
		       me.name, parv[0], buf);
	    (void) strncpy(buf, "* ", 3);
	    (void) strncpy(buf + 2, chptr->chname, len + 1);
	    (void) strcat(buf, " :");
	    if (PubChannel(chptr))
	       *buf = '=';
	    else if (SecretChannel(chptr))
	       *buf = '@';
	    idx = len + 4;
	    flag = 0;
	 }
      }
      if (flag)
	 sendto_one(sptr, rpl_str(RPL_NAMREPLY),
		    me.name, parv[0], buf);
   }
   if (!BadPtr(para)) {
      sendto_one(sptr, rpl_str(RPL_ENDOFNAMES), me.name, parv[0],
		 para);
      return (1);
   }

   /*
    * Second, do all non-public, non-secret channels in one big sweep 
    */

   (void) strncpy(buf, "* * :", 6);
   idx = 5;
   flag = 0;
   for (c2ptr = client; c2ptr; c2ptr = c2ptr->next) {
   aChannel   *ch3ptr;
   int         showflag = 0, secret = 0;

      if (!IsPerson(c2ptr) || IsInvisible(c2ptr))
	 continue;
      lp = c2ptr->user->channel;
      /*
       * dont show a client if they are on a secret channel or they are
       * on a channel sptr is on since they have already been show
       * earlier. -avalon
       */
      while (lp) {
	 ch3ptr = lp->value.chptr;
	 if (PubChannel(ch3ptr) || IsMember(sptr, ch3ptr))
	    showflag = 1;
	 if (SecretChannel(ch3ptr))
	    secret = 1;
	 lp = lp->next;
      }
      if (showflag)		/*
				 * have we already shown them ? 
				 */
	 continue;
      if (secret)		/*
				 * on any secret channels ? 
				 */
	 continue;
      (void) strncat(buf, c2ptr->name, NICKLEN);
      idx += strlen(c2ptr->name) + 1;
      (void) strcat(buf, " ");
      flag = 1;
      if (mlen + idx + NICKLEN > BUFSIZE - 3) {
	 sendto_one(sptr, rpl_str(RPL_NAMREPLY),
		    me.name, parv[0], buf);
	 (void) strncpy(buf, "* * :", 6);
	 idx = 5;
	 flag = 0;
      }
   }
   if (flag)
      sendto_one(sptr, rpl_str(RPL_NAMREPLY), me.name, parv[0], buf);
   sendto_one(sptr, rpl_str(RPL_ENDOFNAMES), me.name, parv[0], "*");
   return (1);
}

void
send_user_joins(aClient *cptr, aClient *user)
{
   Reg Link   *lp;
   Reg aChannel *chptr;
   Reg int     cnt = 0, len = 0, clen;
   char       *mask;

   *buf = ':';
   (void) strcpy(buf + 1, user->name);
   (void) strcat(buf, " JOIN ");
   len = strlen(user->name) + 7;

   for (lp = user->user->channel; lp; lp = lp->next) {
      chptr = lp->value.chptr;
      if (*chptr->chname == '&')
	 continue;
      if ((mask = strchr(chptr->chname, ':')))
	 if (match(++mask, cptr->name))
	    continue;
      clen = strlen(chptr->chname);
      if (clen > (size_t) BUFSIZE - 7 - len) {
	 if (cnt)
	    sendto_one(cptr, "%s", buf);
	 *buf = ':';
	 (void) strcpy(buf + 1, user->name);
	 (void) strcat(buf, " JOIN ");
	 len = strlen(user->name) + 7;
	 cnt = 0;
      }
      (void) strcpy(buf + len, chptr->chname);
      cnt++;
      len += clen;
      if (lp->next) {
	 len++;
	 (void) strcat(buf, ",");
      }
   }
   if (*buf && cnt)
      sendto_one(cptr, "%s", buf);

   return;
}

static void
sjoin_sendit(aClient *cptr,
	     aClient *sptr,
	     aChannel *chptr,
	     char *from)
{
   sendto_channel_butserv(chptr, sptr, ":%s MODE %s %s %s", from,
			  chptr->chname, modebuf, parabuf);
}

/*
 * m_sjoin parv[0] - sender parv[1] - TS parv[2] - channel parv[3] -
 * modes + n arguments (key and/or limit) parv[4+n] - flags+nick list
 * (all in one parameter)
 * 
 * 
 * process a SJOIN, taking the TS's into account to either ignore the
 * incoming modes or undo the existing ones or merge them, and JOIN all
 * the specified users while sending JOIN/MODEs to non-TS servers and
 * to clients
 */

#define INSERTSIGN(x,y) \
if (what != x) { \
*mbuf++=y; \
what = x; \
}
	
int
m_sjoin(aClient *cptr,
	aClient *sptr,
	int parc,
	char *parv[])
{
   aChannel   *chptr;
   aClient    *acptr;
   ts_val      newts, oldts, tstosend;
   static Mode mode, *oldmode;
   Link       *l;
   int         args = 0, haveops = 0, keepourmodes = 1, keepnewmodes = 1,
	doesop = 0, what = 0, pargs = 0, fl, people = 0,
	isnew;
   Reg char   *s, *s0;
   static char numeric[16], sjbuf[BUFSIZE];
   char       *mbuf = modebuf, *t = sjbuf, *p;
	time_t      creation=0;
	
   if (IsClient(sptr) || parc < 6)
	  return 0;
   if (!IsChannelName(parv[3]))
	  return 0;
   newts = atol(parv[1]);
   creation = atol(parv[2]);
	 memset((char *) &mode, '\0', sizeof(mode));
	
   s = parv[4];
   while (*s)
	  switch (*(s++)) {
		case 'i':
		  mode.mode |= MODE_INVITEONLY;
		  break;
		case 'n':
		  mode.mode |= MODE_NOPRIVMSGS;
		  break;
		case 'p':
		  mode.mode |= MODE_PRIVATE;
		  break;
		case 's':
		  mode.mode |= MODE_SECRET;
		  break;
		case 'm':
		  mode.mode |= MODE_MODERATED;
		  break;
		case 't':
		  mode.mode |= MODE_TOPICLIMIT;
		  break;
		case 'r':
		  mode.mode |= MODE_REGISTERED;
		  break;
		case 'R':
		  mode.mode |= MODE_REGONLY;
		  break;
		case 'k':
		  strncpyzt(mode.key, parv[5 + args], KEYLEN + 1);
		  args++;
		  if (parc < 6 + args)
	       return 0;
		  break;
		case 'l':
		  mode.limit = atoi(parv[5 + args]);
		  args++;
		  if (parc < 6 + args)
	       return 0;
		  break;
	  }
	
   *parabuf = '\0';
	
   isnew = ChannelExists(parv[3]) ? 0 : 1;
   chptr = get_channel(sptr, parv[3], CREATE);
   oldts = chptr->channelts;
   doesop = (parv[5 + args][0] == '@' || parv[5 + args][1] == '@');
	if(chptr->creationtime>creation) /* pick the oldest creation time, and set it */
	  chptr->creationtime=creation;
	
   for (l = chptr->members; l && l->value.cptr; l = l->next)
	  if (l->flags & MODE_CHANOP) {
		  haveops++;
		  break;
	  }
	
   oldmode = &chptr->mode;
	
   if (isnew)
	  chptr->channelts = tstosend = newts;
   else if (newts == 0 || oldts == 0)
	  chptr->channelts = tstosend = 0;
   else if (newts == oldts)
	  tstosend = oldts;
   else if (newts < oldts) {
      if (doesop)
		  keepourmodes = 0;
      if (haveops && !doesop)
		  tstosend = oldts;
      else
		  chptr->channelts = tstosend = newts;
   }
   else {
      if (haveops)
		  keepnewmodes = 0;
      if (doesop && !haveops) {
			chptr->channelts = tstosend = newts;
			if (MyConnect(sptr) && !IsULine(sptr))
			  ts_warn("Hacked ops on opless channel: %s",
						 chptr->chname);
      }
      else
		  tstosend = oldts;
   }
	
   if (!keepnewmodes)
	  mode = *oldmode;
   else if (keepourmodes) {
      mode.mode |= oldmode->mode;
      if (oldmode->limit > mode.limit)
		  mode.limit = oldmode->limit;
      if (strcmp(mode.key, oldmode->key) < 0)
		  strcpy(mode.key, oldmode->key);
   }
	
	/* plus modes */
   if((MODE_PRIVATE & mode.mode) && !(MODE_PRIVATE & oldmode->mode)) {
		INSERTSIGN(1,'+')
		*mbuf++ = 'p';
	}
	if((MODE_SECRET & mode.mode) && !(MODE_SECRET & oldmode->mode)) {
		INSERTSIGN(1,'+')
		*mbuf++ = 's';
	}
	if((MODE_MODERATED & mode.mode) && !(MODE_MODERATED & oldmode->mode)) {
		INSERTSIGN(1,'+')
		*mbuf++ = 'm';
	}
	if((MODE_NOPRIVMSGS & mode.mode) && !(MODE_NOPRIVMSGS & oldmode->mode)) {
		INSERTSIGN(1,'+')
		*mbuf++ = 'n';
	}
	if((MODE_TOPICLIMIT & mode.mode) && !(MODE_TOPICLIMIT & oldmode->mode)) {
		INSERTSIGN(1,'+')
		*mbuf++ = 't';
	}
	if((MODE_INVITEONLY & mode.mode) && !(MODE_INVITEONLY & oldmode->mode)) {
		INSERTSIGN(1,'+')
		*mbuf++ = 'i';
	}
	if((MODE_REGISTERED & mode.mode) && !(MODE_REGISTERED & oldmode->mode)) {
		INSERTSIGN(1,'+')
		*mbuf++='r';
	}
	if((MODE_REGONLY & mode.mode) && !(MODE_REGONLY & oldmode->mode)) {
		INSERTSIGN(1,'+')
		*mbuf++='R';
	}
   
	/* minus modes */
	if((MODE_PRIVATE & mode.mode) && !(MODE_PRIVATE & oldmode->mode)) {
		INSERTSIGN(-1,'-')
		*mbuf++ = 'p';
	}
	if((MODE_SECRET & mode.mode) && !(MODE_SECRET & oldmode->mode)) {
		INSERTSIGN(-1,'-')
		*mbuf++ = 's';
	}
	if((MODE_MODERATED & mode.mode) && !(MODE_MODERATED & oldmode->mode)) {
		INSERTSIGN(-1,'-')
		*mbuf++ = 'm';
	}
	if((MODE_NOPRIVMSGS & mode.mode) && !(MODE_NOPRIVMSGS & oldmode->mode)) {
		INSERTSIGN(-1,'-')
		*mbuf++ = 'n';
	}
	if((MODE_TOPICLIMIT & mode.mode) && !(MODE_TOPICLIMIT & oldmode->mode)) {
		INSERTSIGN(-1,'-')
		*mbuf++ = 't';
	}
	if((MODE_INVITEONLY & mode.mode) && !(MODE_INVITEONLY & oldmode->mode)) {
		INSERTSIGN(-1,'-')
		*mbuf++ = 'i';
	}
	if((MODE_REGISTERED & mode.mode) && !(MODE_REGISTERED & oldmode->mode)) {
		INSERTSIGN(-1,'-')
		*mbuf++='r';
	}
	if((MODE_REGONLY & mode.mode) && !(MODE_REGONLY & oldmode->mode)) {
		INSERTSIGN(-1,'-')
		*mbuf++='R';
	}
	
	if (oldmode->limit && !mode.limit) {
		INSERTSIGN(-1,'-')
      *mbuf++ = 'l';
   }
   if (oldmode->key[0] && !mode.key[0]) {
		INSERTSIGN(-1,'-')
      *mbuf++ = 'k';
      strcat(parabuf, oldmode->key);
      strcat(parabuf, " ");
      pargs++;
   }
   if (mode.limit && oldmode->limit != mode.limit) {
		INSERTSIGN(1,'+')
      *mbuf++ = 'l';
      (void) sprintf(numeric, "%-15d", mode.limit);
      if ((s = strchr(numeric, ' ')))
		  *s = '\0';
      strcat(parabuf, numeric);
      strcat(parabuf, " ");
      pargs++;
   }
   if (mode.key[0] && strcmp(oldmode->key, mode.key)) {
		INSERTSIGN(1,'+')
      *mbuf++ = 'k';
      strcat(parabuf, mode.key);
      strcat(parabuf, " ");
      pargs++;
   }
	
   chptr->mode = mode;
	
   if (!keepourmodes) {
      what = 0;
      for (l = chptr->members; l && l->value.cptr; l = l->next) {
			if (l->flags & MODE_CHANOP) {
				INSERTSIGN(-1,'-')
				  *mbuf++ = 'o';
				strcat(parabuf, l->value.cptr->name);
				strcat(parabuf, " ");
				pargs++;
				if (pargs >= (MAXMODEPARAMS - 2)) {
					*mbuf = '\0';
					sjoin_sendit(cptr, sptr, chptr,
									 parv[0]);
					mbuf = modebuf;
					*mbuf = parabuf[0] = '\0';
					pargs = what = 0;
				}
				l->flags &= ~MODE_CHANOP;
			}
			if (l->flags & MODE_VOICE) {
				INSERTSIGN(-1,'-')
				*mbuf++ = 'v';
				strcat(parabuf, l->value.cptr->name);
				strcat(parabuf, " ");
				pargs++;
				if (pargs >= (MAXMODEPARAMS - 2)) {
					*mbuf = '\0';
					sjoin_sendit(cptr, sptr, chptr,
									 parv[0]);
					mbuf = modebuf;
					*mbuf = parabuf[0] = '\0';
					pargs = what = 0;
				}
				l->flags &= ~MODE_VOICE;
			}
      }
      sendto_channel_butserv(chptr, &me,
									  ":%s NOTICE %s :*** Notice -- TS for %s changed from %ld to %ld",
									  me.name, chptr->chname, chptr->chname, oldts, newts);
   }
   if (mbuf != modebuf) {
      *mbuf = '\0';
      sjoin_sendit(cptr, sptr, chptr, parv[0]);
   }
	
   *modebuf = *parabuf = '\0';
   if (parv[4][0] != '0' && keepnewmodes)
	  channel_modes(sptr, modebuf, parabuf, chptr);
   else {
      modebuf[0] = '0';
      modebuf[1] = '\0';
   }
	
   sprintf(t, ":%s SJOIN %ld %ld %s %s %s :", parv[0], tstosend, creation,
			  parv[3], modebuf, parabuf);
   t += strlen(t);
	
   mbuf = modebuf;
   parabuf[0] = '\0';
   pargs = 0;
   *mbuf++ = '+';
	
   for (s = s0 = strtoken(&p, parv[args + 5], " "); s;
		  s = s0 = strtoken(&p, (char *) NULL, " ")) {
      fl = 0;
      if (*s == '@' || s[1] == '@')
		  fl |= MODE_CHANOP;
      if (*s == '+' || s[1] == '+')
		  fl |= MODE_VOICE;
      if (!keepnewmodes) {
			if (fl & MODE_CHANOP)
			  fl = MODE_DEOPPED;
			else
			  fl = 0;
		}
      while (*s == '@' || *s == '+')
		  s++;
      if (!(acptr = find_chasing(sptr, s, NULL)))
		  continue;
      if (acptr->from != cptr)
		  continue;
      people++;
      if (!IsMember(acptr, chptr)) {
			add_user_to_channel(chptr, acptr, fl);
			sendto_channel_butserv(chptr, acptr, ":%s JOIN :%s",
										  s, parv[3]);
      }
      if (keepnewmodes)
		  strcpy(t, s0);
      else
		  strcpy(t, s);
      t += strlen(t);
      *t++ = ' ';
      if (fl & MODE_CHANOP) {
			*mbuf++ = 'o';
			strcat(parabuf, s);
			strcat(parabuf, " ");
			pargs++;
			if (pargs >= (MAXMODEPARAMS - 3)) {
				*mbuf = '\0';
				sjoin_sendit(cptr, sptr, chptr, parv[0]);
				mbuf = modebuf;
				*mbuf++ = '+';
				parabuf[0] = '\0';
				pargs = 0;
			}
      }
      if (fl & MODE_VOICE) {
			*mbuf++ = 'v';
			strcat(parabuf, s);
			strcat(parabuf, " ");
			pargs++;
			if (pargs >= (MAXMODEPARAMS - 3)) {
				*mbuf = '\0';
				sjoin_sendit(cptr, sptr, chptr, parv[0]);
				mbuf = modebuf;
				*mbuf++ = '+';
				parabuf[0] = '\0';
				pargs = 0;
			}
      }
   }
	
   *mbuf = '\0';
   if (pargs)
	  sjoin_sendit(cptr, sptr, chptr, parv[0]);
   if (people) {
      if (t[-1] == ' ')
		  t[-1] = '\0';
      else
		  *t = '\0';
      sendto_match_servs(chptr, cptr, "%s", sjbuf);
   }
   return 0;
}
#undef INSERTSIGN

/* m_samode - Just bout the same as df
 *  - Raistlin 
 * parv[0] = sender
 * parv[1] = channel
 * parv[2] = modes
 */
int m_samode(aClient *cptr, aClient *sptr, int parc, char *parv[]) {
	int sendts;
	aChannel *chptr;
	if (check_registered(cptr)) return 0;
	if (!IsPrivileged(cptr)) {
		sendto_one(sptr, err_str(ERR_NOPRIVILEGES), me.name, parv[0]);
		return 0;
	}
	if (!IsSAdmin(cptr)||parc<2) return 0;
	chptr=find_channel(parv[1], NullChn);
	if (chptr==NullChn) return 0;
	if(!check_channelname(sptr, (unsigned char *)parv[1]))
	  return 0;
	clean_channelname((unsigned char *) parv[1]);
	sendts = set_mode(cptr, sptr, chptr, 2, parc - 2, parv + 2, modebuf, 
							parabuf);
	
	if (strlen(modebuf) > (size_t)1)
	  {
		  sendto_channel_butserv(chptr, sptr, ":%s MODE %s %s %s",
										 me.name, chptr->chname, modebuf, parabuf);
		  sendto_match_servs(chptr, cptr, ":%s MODE %s %s %s",
									parv[0], chptr->chname, modebuf, parabuf);
		  if(MyClient(sptr)) {
			  sendto_serv_butone(&me, ":%s GLOBOPS :%s used SAMODE (%s %s%s%s)",
										me.name, sptr->name, chptr->chname, modebuf,
										(*parabuf!=0 ? " " : ""), parabuf);
			  send_globops("from %s: %s used SAMODE (%s %s%s%s)",
								me.name, sptr->name, chptr->chname, modebuf, 
								(*parabuf!=0 ? " " : ""), parabuf);
		  }
	  }
	return 0;
}

char  *pretty_mask(char *mask)
{
	register  char  *cp, *user, *host;
	
	if ((user = strchr((cp = mask), '!')))
	  *user++ = '\0';
	if ((host = strrchr(user ? user : cp, '@')))
	  {
		  *host++ = '\0';
		  if (!user)
			 return make_nick_user_host(NULL, cp, host);
	  }
	else if (!user && strchr(cp, '.'))
	  return make_nick_user_host(NULL, NULL, cp);
	return make_nick_user_host(cp, user, host);
}