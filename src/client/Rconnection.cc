/*
 *  C++ Interface to Rserve
 *  Copyright (C) 2004 Simon Urbanek, All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  $Id$
 */

/* external defines:
   SWAPEND  - needs to be defined for platforms with inverse endianess related to Intel
   see also SOCK_ERROR, MAIN and other defines in sisocks.h
*/

#include <Rconnection.h>

#include <stdio.h>
#include <sisocks.h>
#ifdef unix
#include <sys/un.h>
#else
#define AF_LOCAL -1
#endif
#include "Rsrv.h"

static char *myID= "Rsrv0102QAP1"; /* this client supports up to protocol version 0102 */

static Rexp *new_parsed_Rexp(unsigned int *d, Rmessage *msg) {
    int type=ptoi(*d)&0x3f;
    if (type==XT_ARRAY_INT || type==XT_INT)
        return new Rinteger(d,msg);
    if (type==XT_ARRAY_DOUBLE || type==XT_DOUBLE)
        return new Rdouble(d,msg);
    if (type==XT_LIST)
        return new Rlist(d,msg);
    if (type==XT_VECTOR)
        return new Rvector(d,msg);
    if (type==XT_STR)
        return new Rstring(d,msg);
    if (type==XT_SYM)
        return new Rsymbol(d,msg);
    return new Rexp(d,msg);
}

static Rexp *new_parsed_Rexp_from_Msg(Rmessage *msg) {
    int hl=1;
    unsigned int *hp=msg->par[0];
    Rsize_t plen=hp[0]>>8;
    if ((hp[0]&DT_LARGE)>0) {
        hl++;
        plen|=((Rsize_t)hp[1])<<24;
    }
    return new_parsed_Rexp(hp+hl,msg);
}    

Rmessage::Rmessage() {
    complete=0;
    data=0;
    len=0;
}

Rmessage::Rmessage(int cmd) {
    memset(&head,0,sizeof(head));
    head.cmd=cmd&0x3f;
    data=0;
    len=0;
    complete=1;
}

Rmessage::Rmessage(int cmd, const char *txt) {
    memset(&head,0,sizeof(head));
    int tl=strlen(txt)+1;
    if ((tl&3)>0)
        tl=(tl+4)&0xffffc; // allign the text
    len=tl+4; // message length is tl + 4 (short format only)
    head.cmd=cmd&0x3f;
    head.len=len;
    data=(char*)malloc(tl+16);
    memset(data,0,tl+16);
    *((int*)data)=itop(SET_PAR(DT_STRING,tl));
    strcpy(data+4,txt);
    complete=1;
}
    
Rmessage::~Rmessage() {
    if(data) free(data);
    complete=0;
}
    
int Rmessage::read(int s) {
    complete=0;
    int n=recv(s,(char*)&head,sizeof(head),0);
    if (n!=sizeof(head)) {
        closesocket(s); s=-1;
        return (n==0)?-7:-8;
    }
    Rsize_t i=len=head.len=ptoi(head.len);        
    head.cmd=ptoi(head.cmd);
    head.dof=ptoi(head.dof);
    head.res=ptoi(head.res);
    if (head.dof>0) { // skip past DOF if present
            char sb[256];
        int k=head.dof;
        while (k>0) {
            n=recv(s,(char*)sb,(k>256)?256:k,0);
            if (n<1) {
                closesocket(s); s=-1;
                return -8; // malformed packet
            }
            k-=n;
        }
    }
    if (i>0) {
        data=(char*) malloc(i);
        if (!data) {
            closesocket(s); s=-1;
            return -10; // out of memory
        }
        char *dp=data;
        while(i>0 && (n=recv(s,(char*)dp,i,0))>0) {
            dp+=n;
            i-=n;
        }
        if (i>0) {
            closesocket(s); s=-1;
            return -8;
        }
    }
    parse();
    complete=1;
    return 0;
}

void Rmessage::parse() {
    pars=0;
    if (len<4) return;
    char *c=data, *eop=c+len;
    while (c<eop) {
        int hs=4;
        unsigned int *pp=(unsigned int*)c;
        unsigned int p1=ptoi(pp[0]);
        
        Rsize_t len=p1>>8;
        if ((p1&DT_LARGE)>0) {
            hs+=4;
            unsigned int p2=ptoi(pp[1]);
            len|=((Rsize_t)p2)<<24;
        }
        //printf("par %d: %x length %d\n", pars, p1&0x3f, len);
        par[pars++]=(unsigned int*)c;
        c+=hs;
        c+=len;
        if (pars>15) break; // max 16 pars
    }
}

int Rmessage::send(int s) {
    int failed=0;
    head.cmd=itop(head.cmd);
    head.len=itop(head.len);
    head.dof=itop(head.dof);
    head.res=itop(head.res);
    if (::send(s,(char*)&head,sizeof(head),0)!=sizeof(head))
        failed=-1;
    if (!failed && len>0 && ::send(s,data,len,0)!=len)
        failed=-1;
    head.cmd=ptoi(head.cmd);
    head.len=ptoi(head.len);
    head.dof=ptoi(head.dof);
    head.res=ptoi(head.res);
    return failed;
}

Rexp::Rexp(Rmessage *msg) {
    //printf("new Rexp@%x\n", this);
    master=0; rcount=0; attr=0; attribs=0;
    this->msg=msg;
    int hl=1;
    unsigned int *hp=msg->par[0];
    Rsize_t plen=hp[0]>>8;
    if ((hp[0]&DT_LARGE)>0) {
        hl++;
        plen|=((Rsize_t)hp[1])<<24;
    }
    next=parse(hp+hl);
}
    
Rexp::Rexp(unsigned int *pos, Rmessage *msg) {
    //printf("new Rexp@%x\n", this);
    attr=0; master=0; this->msg=msg; rcount=0; attribs=0;
    next=parse(pos);
}

Rexp::Rexp(int type, char *data, int len, Rexp *attr) {
    this->attr=attr; master=this; rcount=0; attribs=0;
    this->type=type;
    if (len>0) {
        this->data=(char*) malloc(len);
        memcpy(this->data, data, len);
        this->len=len;
    } else
        this->len=0;
    next=data+this->len;
}

Rexp::~Rexp() {
    //printf("releasing Rexp@%x\n", this);
    if (attr)
        delete(attr);
    attr=0;
    if (master) {
        if (master==this) {
            free(data); len=0;
        } else
            master->rcount--;
        master=0;
    }
    if (msg) {
        if (rcount>0)
            fprintf(stderr, "WARNING! Rexp master %x delete requested, but %d object(s) are using our memory - refusing to free, leaking...\n", this, rcount);
        else
            delete(msg);
    }
    msg=0;
}

void Rexp::set_master(Rexp *m) {
    if (master) master->rcount--;
    master=m;
    if (m) m->rcount++;
}
    
char *Rexp::parse(unsigned int *pos) { // plen is not used
    this->pos=pos;
    int hl=1;
    unsigned int p1=ptoi(pos[0]);
    len=p1>>8;
    if ((p1&XT_LARGE)>0) {
        hl++;
        len|=((Rsize_t)(ptoi(pos[1])))<<24;
    }
    data=(char*)(pos+hl);
    if (p1&XT_HAS_ATTR) {
        attr=new_parsed_Rexp((unsigned int*)data, 0);
        len-=attr->next-data;
        data=attr->next;
        if (master || msg)
            attr->set_master(master?master:this);
    }
    type=p1&0x3f;
    //printf("Rexp(type=%d, len=%d, attr=%x)\n", type, len, attr);
    return data+len;
}

void Rexp::store(char *buf) {
    int hl=4;
    unsigned int *i = (unsigned int*)buf;
    i[0]=SET_PAR(type, len);
    i[0]=itop(i[0]);
    if (len>0x7fffff) {
        buf[0]|=XT_LARGE;
        i[1]=itop(len>>24);
        hl+=4;
    }
    memcpy(buf+hl, data, len);
}

Rexp *Rexp::attribute(const char *name) {
    return (attr && attr->type==XT_LIST)?((Rlist*)attr)->entryByTagName(name):0;
}

char **Rexp::attributeNames() {
    if (!attr || attr->type!=XT_LIST)
        return 0;
    if (attribs==0) {
        // let us cache attribute names
        Rlist *l = (Rlist*) attr;
        while (l && l->type==XT_LIST) {
            if (l->tag && l->tag->type==XT_SYM) attribs++;
            l=l->tail;
        }
        attrnames=(char**) malloc(sizeof(char*)*(attribs+1));
        l = (Rlist*) attr;
        while (l && l->type==XT_LIST) {
            if (l->tag && l->tag->type==XT_SYM)
                attrnames[attribs++]=((Rsymbol*)l->tag)->symbolName();
            l=l->tail;
        }
        attrnames[attribs]=0;
    }
    return attrnames;
}

void Rinteger::fix_content() {
    if (len<0 || !data) return;
#ifdef SWAPEND
    int *i = (int*) data;
    int *j = (int*) (data+len);
    while (i<j) { *i=ptoi(*i); i++; }
#endif
}

void Rdouble::fix_content() {
    if (len<0 || !data) return;
#ifdef SWAPEND
    double *i = (double*) data;
    double *j = (double*) (data+len);
    while (i<j) { *i=ptod(*i); i++; }
#endif
}

void Rsymbol::fix_content() {
    if (*data==3) name=data+4; // normally the symbol should consist of a string SEXP specifying its name - no further content is defined as of now
}

Rlist::~Rlist() {
    if (head) delete(head);
    if (tail) delete(tail);
    if (tag) delete(tag);
}

void Rlist::fix_content() {
    char *ptr = data;
    char *eod = data+len;
    head=new_parsed_Rexp((unsigned int*)ptr,0);
    if (head) {
        ptr=head->next;
        if (ptr<eod) {
            tail=(Rlist*)new_parsed_Rexp((unsigned int*)ptr,0);
            if (tail) {
                ptr=tail->next;
                if (ptr<eod)
                    tag=new_parsed_Rexp((unsigned int*)ptr,0);
                if (tail->type!=XT_LIST) {
                    // if tail is not a list, then something is wrong - just delete it
                    delete(tail); tail=0;
                }
            }
        }
    }
}

Rvector::~Rvector() {
    int i=0;
    while(i<count) {
        if (cont[i]) delete(cont[i]);
        i++;
    }
    if (strs) free(strs);
    free(cont);
}

char **Rvector::strings() {
    if (strs) return strs;
    int i=0, sc=0;
    while (i<count) {
        if (cont[i] && cont[i]->type==XT_STR) sc++;
        i++;
    }
    if (sc==0) return 0;
    strs=(char**)malloc(sizeof(char*)*(sc+1));
    i=0; sc=0;
    while (i<count) {
        if (cont[i] && cont[i]->type==XT_STR) strs[sc++]=((Rstring*)cont[i])->string();
        i++;
    }
    strs[sc]=0;
    return strs;
}

int Rvector::indexOf(Rexp *exp) {
    int i=0;
    while (i<count) {
        if (cont[i]==exp) return i;
        i++;
    }
    return -1;
}

int Rvector::indexOfString(const char *str) {
    int i=0;
    while (i<count) {
        if (cont[i] && cont[i]->type==XT_STR && !strcmp(((Rstring*)cont[i])->string(),str)) return i;
        i++;
    }
    return -1;
}

Rexp* Rvector::byName(const char *name) {
    if (count<1 || !attr || attr->type!=XT_LIST) return 0;        
    Rexp *e = ((Rlist*) attr)->head;
    if (((Rlist*) attr)->tag)
        e=((Rlist*) attr)->entryByTagName("names");
    if (!e || (e->type!=XT_VECTOR && e->type!=XT_STR))
        return 0;
    if (e->type==XT_VECTOR) {
        int pos = ((Rvector*)e)->indexOfString(name);
        if (pos>-1 && pos<count) return cont[pos];
    } else {
        if (!strcmp(((Rstring*)e)->string(),name))
            return cont[0];
    }
    return 0;
}
       
void Rvector::fix_content() {
    char *ptr = data;
    char *eod = data+len;
    capacity=16;
    cont=(Rexp**) malloc(sizeof(Rexp*)*capacity);
    while (ptr<eod) {
        if (count==capacity) {
            capacity*=2;
            cont=(Rexp**) realloc(cont, sizeof(Rexp*)*capacity);
        }
        cont[count]=new_parsed_Rexp((unsigned int*)ptr,0);
        if (cont[count])
            ptr=cont[count]->next;
        else break;
        count++;
    }
}
    
Rconnection::Rconnection(char *host, int port) {
    if (!host) host="127.0.0.1";
    this->host=(char*)malloc(strlen(host)+1);
    strcpy(this->host, host);
    this->port=port;
    family=(port==-1)?AF_LOCAL:AF_INET;
    s=-1;
}
    
Rconnection::~Rconnection() {
    if (host) free(host); host=0;
    if (s!=-1) closesocket(s);
    s=-1;
}
    
int Rconnection::connect() {
#ifdef unix
    struct sockaddr_un sau;
#endif
    SAIN sai;
    char IDstring[33];
    
    if (family==AF_INET) {
        memset(&sai,0,sizeof(sai));
        build_sin(&sai,host,port);
    } else {
#ifdef unix
        memset(&sau,0,sizeof(sau));
        sau.sun_family=AF_LOCAL;
        strcpy(sau.sun_path,host); // FIXME: possible overflow!
#else
	return -11;  // unsupported
#endif
    }
    
    IDstring[32]=0;
    int i;
    
    s=socket(family,SOCK_STREAM,0);
    if (family==AF_INET)
        i=::connect(s,(SA*)&sai,sizeof(sai));
#ifdef unix
    else
        i=::connect(s,(SA*)&sau,sizeof(sau));
#endif
    if (i==-1) {
        closesocket(s); s=-1;
        return -1; // connect failed
    }
    int n=recv(s,IDstring,32,0);
    if (n!=32) {
        closesocket(s); s=-1;
        return -2; // handshake failed (no IDstring)
    }
    if (strncmp(IDstring,myID,4)) {
        closesocket(s); s=-1;
        return -3; // invalid IDstring
    }
    if (strncmp(IDstring+8,myID+8,4) || strncmp(IDstring+4,myID+4,4)>0) {
        closesocket(s); s=-1;
        return -4; // protocol not supported
    }
    return 0;
}

int Rconnection::disconnect() {
    if (s>-1) {
        closesocket(s);
        s=-1;
    }
}

/**--- low-level functions --*/

int Rconnection::request(Rmessage *msg, int cmd, int len, void *par) {
    struct phdr ph;
    int i,n,j;
    char buf[128];
    
    if (s==-1) return -5; // not connected
    memset(&ph,0,sizeof(ph));
    ph.len=itop(len);
    ph.cmd=itop(cmd);
    if (send(s,(char*)&ph,sizeof(ph),0)!=sizeof(ph)) {
        closesocket(s); s=-1;
        return -9;
    }
    if (len>0 && send(s,(char*)par,len,0)!=len) {
        closesocket(s); s=-1;
        return -9;
    }
    return msg->read(s);
}

int Rconnection::request(Rmessage *targetMsg, Rmessage *contents) {
    if (s==-1) return -5; // not connected
    if (contents->send(s)) {
        closesocket(s); s=-1;
        return -9; // send error
    }
    return targetMsg->read(s);
}

/** --- high-level functions -- */

int Rconnection::assign(const char *symbol, Rexp *exp) {
    Rmessage *msg=new Rmessage();
    Rmessage *cm=new Rmessage(CMD_setSEXP);
    
    int tl=strlen(symbol)+1;
    if (tl&3) tl=(tl+4)&0xfffc;
    Rsize_t xl=exp->storageSize();
    Rsize_t hl=4+tl+4;
    if (xl>0x7fffff) hl+=4;
    cm->data=(char*) malloc(hl+xl);
    cm->head.len=cm->len=hl+xl;
    ((unsigned int*)cm->data)[0]=SET_PAR(DT_STRING, tl);
    ((unsigned int*)cm->data)[0]=itop(((unsigned int*)cm->data)[0]);
    strcpy(cm->data+4, symbol);
    ((unsigned int*)(cm->data+4+tl))[0]=SET_PAR((Rsize_t) ((xl>0x7fffff)?(DT_SEXP|DT_LARGE):DT_SEXP), (Rsize_t) xl);
    ((unsigned int*)(cm->data+4+tl))[0]=itop(((unsigned int*)(cm->data+4+tl))[0]);
    if (xl>0x7fffff)
        ((unsigned int*)(cm->data+4+tl))[1]=itop(xl>>24);
    exp->store(cm->data+hl);
    
    int res=request(msg,cm);
    delete (cm);
    if (res) {
        delete(msg);
        return res;
    }
    // we should check response code here ...
    return 0;
}

int Rconnection::voidEval(const char *cmd) {
    int status=0;
    eval(cmd, &status, 1);
    return status;
}
    
Rexp *Rconnection::eval(const char *cmd, int *status, int opt) {
    Rmessage *msg=new Rmessage();
    Rmessage *cmdMessage=new Rmessage((opt&1)?CMD_voidEval:CMD_eval, cmd);
    int res=request(msg,cmdMessage);
    delete (cmdMessage);
    if (opt&1 && !res) {
        if (status) *status=0; // we should put response code here
        delete(msg);
        return 0;
    }
    if (!res && (msg->pars!=1 || (ptoi(msg->par[0][0])&0x3f)!=DT_SEXP)) {
        delete(msg);
        if (status) *status=-10; // returned object is not SEXP
        return 0;
    }
    if (res) {
        delete(msg);
        if (status) *status=res;
        return 0;
    }
    if (status) *status=0;
    return new_parsed_Rexp_from_Msg(msg);
}