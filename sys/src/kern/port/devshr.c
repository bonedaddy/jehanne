/*
 * This file is part of Jehanne.
 *
 * Copyright (C) 2015-2016 Giacomo Tesio <giacomo@tesio.it>
 *
 * Jehanne is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of the License.
 *
 * Jehanne is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Jehanne.  If not, see <http://www.gnu.org/licenses/>.
 */

/* This device has been ported from 9front.
 */
#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include	"../port/error.h"

extern Dev* devtab[];

enum {
	Qroot,
	Qcroot,
	Qshr,
	Qcshr,
	Qcmpt,
};

typedef struct Ent Ent;
typedef struct Shr Shr;
typedef struct Mpt Mpt;
typedef struct Sch Sch;

struct Ent
{
	Ref	ref;
	int	id;
	char	*name;
	char	*owner;
	uint32_t	perm;
};

struct Shr
{
	Ent;
	Mhead	umh;
	Shr	*next;
};

struct Mpt
{
	Ent;
	Mount	m;
};

struct Sch
{
	int	level;

	Shr	*shr;
	Mpt	*mpt;

	Chan	*chan;
};

static QLock	shrslk;
static Shr	*shrs;

static int	shrid;
static int	mptid;

static Mpt*
tompt(Mount *m)
{
	return (Mpt*)((char*)m - (char*)&((Mpt*)0)->m);
}

static Sch*
tosch(Chan *c)
{
	Sch *sch;

	if(c == nil)
		error("nil chan");
	sch = c->aux;
	if(sch == nil)
		error("nil chan aux");
	if(sch->chan != c)
		error("bad chan");
	return sch;
}

static void
shrinit(void)
{
	shrid = 1;
	mptid = 1;
}

static void
putmpt(Mpt *mpt)
{
	if(decref(&mpt->ref))
		return;
	if(mpt->m.to != nil)
		cclose(mpt->m.to);
	free(mpt->name);
	free(mpt->owner);
	free(mpt);
}

static void
putshr(Shr *shr)
{
	if(decref(&shr->ref))
		return;
	free(shr->name);
	free(shr->owner);
	free(shr);
}

static Qid
shrqid(int level, int id)
{
	Qid q;

	q.type = (level == Qcmpt) ? QTFILE : QTDIR;
	q.path = (uint64_t)id<<4 | level;
	q.vers = 0;
	return q;
}

static Chan*
shrattach(Chan *c, Chan *ac, char *spec, int flags)
{
	Sch *sch;

	if(!(spec[0] == 'c' && spec[1] == 0 || spec[0] == 0))
		error(Enoattach);
	c = devattach(L'σ', spec);

	sch = smalloc(sizeof(*sch));
	sch->level = spec[0] == 'c' ? Qcroot : Qroot;
	sch->mpt = nil;
	sch->shr = nil;
	sch->chan = c;
	c->aux = sch;
	c->qid = shrqid(sch->level, 0);

	return c;
}

static Chan*
shrclone(Chan *c)
{
	Chan *nc;
	Sch *sch, *och;

	och = tosch(c);
	nc = devclone(c);
	nc->dev = c->dev; 	// devclone does not fill ->dev
				// this is a plan9-9k specific behaviour, but why?
				// many related places are marked with XDYNX
	sch = smalloc(sizeof(*sch));
	memmove(sch, och, sizeof(*sch));
	if(sch->shr != nil)
		incref(&sch->shr->ref);
	if(sch->mpt != nil)
		incref(&sch->mpt->ref);
	sch->chan = nc;
	nc->aux = sch;
	return nc;
}

static void
shrclunk(Chan *c)
{
	Sch *sch;

	sch = tosch(c);
	c->aux = nil;
	sch->chan = nil;
	if(sch->mpt != nil)
		putmpt(sch->mpt);
	if(sch->shr != nil)
		putshr(sch->shr);
	free(sch);
}

static Walkqid*
shrwalk(Chan *c, Chan *nc, char **name, int nname)
{
	Walkqid *wq, *wq2;
	int alloc, j;
	char *nam;
	Sch *sch;
	Shr *shr;
	Mpt *mpt;
	Mount *m;
	Mhead *h;

	alloc = 0;
	if(nc == nil){
		nc = shrclone(c);
		alloc = 1;
	}
	wq = smalloc(sizeof(Walkqid) + (nname - 1) * sizeof(Qid));
	wq->nqid = 0;
	wq->clone = nc;
	if(waserror()){
		if(alloc)
			cclose(wq->clone);
		if(wq->nqid > 0)
			wq->clone = nil;
		else {
			free(wq);
			wq = nil;
		}
		return wq;
	}
	sch = tosch(nc);
	for(j = 0; j < nname; j++){
		if(nc->qid.type != QTDIR)
			error(Enotdir);

		nam = name[j];
		if(nam[0] == '.' && nam[1] == 0) {
			/* nop */
		} else if(nam[0] == '.' && nam[1] == '.' && nam[2] == 0) {
			switch(sch->level){
			default:
				error(Egreg);
			case Qshr:
				nc->qid = shrqid(sch->level = Qroot, 0);
				break;
			case Qcshr:
				nc->qid = shrqid(sch->level = Qcroot, 0);
				break;
			}
			putshr(sch->shr);
			sch->shr = nil;
		} else if(sch->level == Qcroot || sch->level == Qroot) {
			qlock(&shrslk);
			for(shr = shrs; shr != nil; shr = shr->next)
				if(strcmp(nam, shr->name) == 0){
					incref(&shr->ref);
					break;
				}
			qunlock(&shrslk);
			if(shr == nil)
				error(Enonexist);
			sch->level = sch->level == Qcroot ? Qcshr : Qshr;
			sch->shr = shr;
			nc->qid = shrqid(sch->level, shr->id);
		} else if(sch->level == Qcshr) {
			mpt = nil;
			shr = sch->shr;
			h = &shr->umh;
			rlock(&h->lock);
			for(m = h->mount; m != nil; m = m->next){
				mpt = tompt(m);
				if(strcmp(nam, mpt->name) == 0){
					incref(&mpt->ref);
					break;
				}
			}
			runlock(&h->lock);
			if(m == nil)
				error(Enonexist);
			sch->mpt = mpt;
			nc->qid = shrqid(sch->level = Qcmpt, mpt->id);
		} else if(sch->level == Qshr) {
			shr = sch->shr;
			h = &shr->umh;
			wq2 = nil;
			rlock(&h->lock);
			for(m = h->mount; m != nil && wq2 == nil; m = m->next){
				if(m->to == nil)
					continue;
				if(waserror())
					continue;
				wq2 = m->to->dev->walk(m->to, nil, name + j, nname - j);
				poperror();
			}
			runlock(&h->lock);
			if(wq2 == nil)
				error(Enonexist);
			memmove(wq->qid + wq->nqid, wq2->qid, wq2->nqid);
			wq->nqid += wq2->nqid;
			if(alloc)
				cclose(wq->clone);
			wq->clone = wq2->clone;
			free(wq2);
			poperror();
			return wq;
		} else
			error(Egreg);
		wq->qid[wq->nqid++] = nc->qid;
	}
	poperror();
	return wq;
}

static int
shrgen(Chan *c, char* _, Dirtab* __, int ___, int s, Dir *dp)
{
	Mpt *mpt;
	Sch *sch;
	Shr *shr;
	Mhead *h;
	Mount *m;

	sch = tosch(c);
	switch(sch->level){
	default:
		return -1;
	case Qroot:
	case Qcroot:
		qlock(&shrslk);
		for(shr = shrs; shr != nil && s > 0; shr = shr->next)
			s--;
		if(shr == nil){
			qunlock(&shrslk);
			return -1;
		}
		kstrcpy(up->genbuf, shr->name, sizeof up->genbuf);
		if(sch->level == Qroot)
			devdir(c, shrqid(Qshr, shr->id), up->genbuf, 0, shr->owner,
				shr->perm & ~0222, dp);
		else
			devdir(c, shrqid(Qcshr, shr->id), up->genbuf, 0, shr->owner,
				shr->perm, dp);
		qunlock(&shrslk);
		return 1;
	case Qcshr:
		shr = sch->shr;
		h = &shr->umh;
		rlock(&h->lock);
		for(m = h->mount; m != nil && s > 0; m = m->next)
			s--;
		if(m == nil){
			runlock(&h->lock);
			return -1;
		}
		mpt = tompt(m);
		kstrcpy(up->genbuf, mpt->name, sizeof up->genbuf);
		devdir(c, shrqid(Qcmpt, mpt->id), up->genbuf, 0, mpt->owner, mpt->perm, dp);
		runlock(&h->lock);
		return 1;
	}
}

static long
shrstat(Chan *c, uint8_t *db, long n)
{
	Sch *sch;
	Dir dir;
	int rc;

	sch = tosch(c);
	switch(sch->level){
	default:
		error(Egreg);
	case Qroot:
		devdir(c, c->qid, "#σ", 0, eve, 0555, &dir);
		break;
	case Qcroot:
		devdir(c, c->qid, "#σc", 0, eve, 0777, &dir);
		break;
	case Qshr:
		devdir(c, c->qid, sch->shr->name, 0, sch->shr->owner, sch->shr->perm & ~0222, &dir);
		break;
	case Qcshr:
		devdir(c, c->qid, sch->shr->name, 0, sch->shr->owner, sch->shr->perm, &dir);
		break;
	case Qcmpt:
		devdir(c, c->qid, sch->mpt->name, 0, sch->mpt->owner, sch->mpt->perm, &dir);
		break;
	}
	rc = convD2M(&dir, db, n);
	if(rc == 0)
		error(Ebadarg);
	return rc;
}

static Chan*
shropen(Chan *c, int omode)
{
	Chan *nc;
	Sch *sch;
	Shr *shr;
	Mpt *mpt;
	int mode;

	if(c->qid.type == QTDIR && omode != OREAD)
		error(Eisdir);

	mode = openmode(omode);
	sch = tosch(c);
	switch(sch->level){
	default:
		error(Egreg);
	case Qroot:
	case Qcroot:
		break;
	case Qshr:
	case Qcshr:
		shr = sch->shr;
		devpermcheck(shr->owner, shr->perm, mode);
		break;
	case Qcmpt:
		if(omode&OTRUNC)
			error(Eexist);
		shr = sch->shr;
		mpt = sch->mpt;
		devpermcheck(mpt->owner, mpt->perm, mode);
		rlock(&shr->umh.lock);
		if(mpt->m.to == nil || mpt->m.to->mchan == nil){
			runlock(&shr->umh.lock);
			error(Eshutdown);
		}
		nc = mpt->m.to->mchan;
		incref(&nc->r);
		runlock(&shr->umh.lock);
		if(mode != nc->mode){
			cclose(nc);
			error(Eperm);
		}
		cclose(c);
		return nc;
	}
	c->mode = mode;
	c->flag |= COPEN;
	c->offset = 0;
	return c;
}

/* chan.c */
Chan* createdir(Chan *c, Mhead *m);

static Chan*
shrcreate(Chan *c, char *name, int omode, int perm)
{
	Sch *sch;
	Shr *shr;
	Mpt *mpt;
	Mhead *h;
	Mount *m;
	Chan *nc;
	int mode;

	mode = openmode(omode);
	sch = tosch(c);
	switch(sch->level){
	case Qcroot:
	case Qcshr:
		if(strcmp(up->user, "none") == 0)
			error(Eperm);
	}
	switch(sch->level){
	default:
		error(Eperm);
	case Qshr:
		incref(&c->r);
		if(waserror()){
			cclose(c);
			nexterror();
		}
		nc = createdir(c, &sch->shr->umh);
		poperror();
		if(waserror()){
			cclose(nc);
			nexterror();
		}
		nc->dev->create(nc, name, omode, perm);
		poperror();
		cclose(c);
		return nc;
	case Qcroot:
		if(up->pgrp->noattach)
			error(Enoattach);
		if((perm & DMDIR) == 0 || mode != OREAD)
			error(Eperm);
		if(strlen(name) >= sizeof(up->genbuf))
			error(Etoolong);
		qlock(&shrslk);
		if(waserror()){
			qunlock(&shrslk);
			nexterror();
		}
		for(shr = shrs; shr != nil; shr = shr->next)
			if(strcmp(name, shr->name) == 0)
				error(Eexist);

		shr = smalloc(sizeof(*shr));
		incref(&shr->ref);
		shr->id = shrid++;

		kstrdup(&shr->name, name);
		kstrdup(&shr->owner, up->user);
		shr->perm = perm;

		incref(&shr->ref);
		shr->next = shrs;
		shrs = shr;

		poperror();
		qunlock(&shrslk);

		c->qid = shrqid(sch->level = Qcshr, shr->id);
		sch->shr = shr;
		break;
	case Qcshr:
		if(up->pgrp->noattach)
			error(Enoattach);
		if((perm & DMDIR) != 0 || mode != OWRITE)
			error(Eperm);

		shr = sch->shr;
		if(strcmp(shr->owner, eve) == 0 && !iseve())
			error(Eperm);
		devpermcheck(shr->owner, shr->perm, ORDWR);

		if(strlen(name) >= sizeof(up->genbuf))
			error(Etoolong);

		h = &shr->umh;
		wlock(&h->lock);
		if(waserror()){
			wunlock(&h->lock);
			nexterror();
		}
		for(m = h->mount; m != nil; m = m->next){
			mpt = tompt(m);
			if(strcmp(name, mpt->name) == 0)
				error(Eexist);
		}

		mpt = smalloc(sizeof(*mpt));
		incref(&mpt->ref);
		mpt->id = mptid++;

		kstrdup(&mpt->name, name);
		kstrdup(&mpt->owner, up->user);
		mpt->perm = perm;

		incref(&mpt->ref);
		mpt->m.mflag = (h->mount == nil) ? MCREATE : 0;
		mpt->m.next = h->mount;
		h->mount = &mpt->m;

		poperror();
		wunlock(&h->lock);

		c->qid = shrqid(sch->level = Qcmpt, mpt->id);
		sch->mpt = mpt;
		break;
	}
	c->flag |= COPEN;
	c->mode = mode;
	return c;
}

static void
shrremove(Chan *c)
{
	Mount *m, **ml;
	Shr *shr, **sl;
	Sch *sch;
	Mpt *mpt;
	Mhead *h;

	sch = tosch(c);
	if(waserror()){
		shrclunk(c);
		nexterror();
	}
	switch(sch->level){
	default:
		error(Eperm);
	case Qcshr:
	case Qcmpt:
		shr = sch->shr;
		if(!iseve()){
			if(strcmp(shr->owner, eve) == 0)
				error(Eperm);
			devpermcheck(shr->owner, shr->perm, ORDWR);
		}
	}
	switch(sch->level){
	case Qcshr:
		h = &shr->umh;
		qlock(&shrslk);
		rlock(&h->lock);
		if(h->mount != nil){
			runlock(&h->lock);
			qunlock(&shrslk);
			error("directory not empty");
		}
		runlock(&h->lock);
		for(sl = &shrs; *sl != nil; sl = &((*sl)->next))
			if(*sl == shr){
				*sl = shr->next;
				shr->next = nil;
				putshr(shr);
				break;
			}
		qunlock(&shrslk);
		break;
	case Qcmpt:
		mpt = sch->mpt;
		m = &mpt->m;
		h = &shr->umh;
		wlock(&h->lock);
		for(ml = &h->mount; *ml != nil; ml = &((*ml)->next))
			if(*ml == m){
				*ml = m->next;
				m->next = nil;
				putmpt(mpt);
				break;
			}
		wunlock(&h->lock);
		break;
	}
	poperror();
	shrclunk(c);
}

static long
shrwstat(Chan *c, uint8_t *dp, long n)
{
	char *strs;
	Mhead *h;
	Sch *sch;
	Ent *ent;
	Dir d;

	strs = smalloc(n);
	if(waserror()){
		free(strs);
		nexterror();
	}
	n = convM2D(dp, n, &d, strs);
	if(n == 0)
		error(Eshortstat);

	h = nil;
	sch = tosch(c);
	switch(sch->level){
	default:
		error(Eperm);
	case Qcshr:
		ent = sch->shr;
		qlock(&shrslk);
		if(waserror()){
			qunlock(&shrslk);
			nexterror();
		}
		break;
	case Qcmpt:
		ent = sch->mpt;
		h = &sch->shr->umh;
		wlock(&h->lock);
		if(waserror()){
			wunlock(&h->lock);
			nexterror();
		}
		break;
	}

	if(strcmp(ent->owner, up->user) && !iseve())
		error(Eperm);

	if(d.name != nil && *d.name && strcmp(ent->name, d.name) != 0) {
		if(strchr(d.name, '/') != nil)
			error(Ebadchar);
		if(strlen(d.name) >= sizeof(up->genbuf))
			error(Etoolong);
		kstrdup(&ent->name, d.name);
	}
	if(d.uid != nil && *d.uid)
		kstrdup(&ent->owner, d.uid);
	if(d.mode != ~0U)
		ent->perm = d.mode & 0777;

	switch(sch->level){
	case Qcshr:
		poperror();
		qunlock(&shrslk);
		break;
	case Qcmpt:
		poperror();
		wunlock(&h->lock);
		break;
	}

	poperror();
	free(strs);

	return n;
}

static long
shrread(Chan *c, void *va, long n, int64_t _)
{
	Mhead *omh;
	Sch *sch;

	sch = tosch(c);
	switch(sch->level){
	default:
		error(Egreg);
	case Qroot:
	case Qcroot:
	case Qcshr:
		return devdirread(c, va, n, 0, 0, shrgen);
	case Qshr:
		omh = c->umh;
		c->umh = &sch->shr->umh;
		if(waserror()){
			c->umh = omh;
			nexterror();
		}
		n = unionread(c, va, n);
		poperror();
		c->umh = omh;
		return n;
	}
}

static long
shrwrite(Chan *c, void *va, long n, int64_t _)
{
	Sch *sch;
	char *buf, *p, *aname;
	int fd;
	Chan *bc, *c0;
	Mhead *h;
	Mount *m;

	if(up->pgrp->noattach)
		error(Enoattach);
	sch = tosch(c);
	if(sch->level != Qcmpt)
		error(Egreg);

	buf = smalloc(n+1);
	if(waserror()){
		free(buf);
		nexterror();
	}
	memmove(buf, va, n);
	buf[n] = 0;

	fd = strtol(buf, &p, 10);
	if(p == buf || (*p != 0 && *p != '\n'))
		error(Ebadarg);
	if(*p == '\n' && *(p+1) != 0)
		aname = p + 1;
	else
		aname = nil;

	bc = fdtochan(fd, ORDWR, 0, 1);
	if(waserror()) {
		cclose(bc);
		nexterror();
	}
	c0 = mntattach(bc, nil, aname, 0);
	poperror();
	cclose(bc);
	poperror();
	free(buf);

	if(c0 == nil)
		error(Egreg);

	m = &sch->mpt->m;
	h = &sch->shr->umh;
	wlock(&h->lock);
	bc = m->to;
	m->to = c0;
	wunlock(&h->lock);

	if(bc != nil)
		cclose(bc);

	return n;
}

static void
shrclose(Chan *c)
{
	if(c->flag & CRCLOSE)
		shrremove(c);
	else
		shrclunk(c);
}

Dev shrdevtab = {
	L'σ',
	"shr",

	devreset,
	shrinit,
	devshutdown,
	shrattach,
	shrwalk,
	shrstat,
	shropen,
	shrcreate,
	shrclose,
	shrread,
	devbread,
	shrwrite,
	devbwrite,
	shrremove,
	shrwstat,
};

static void
chowner(Ent *ent, char *old, char *new)
{
	if(ent->owner != nil && strcmp(old, ent->owner) == 0)
		kstrdup(&ent->owner, new);
}

void
shrrenameuser(char *old, char *new)
{
	Shr *shr;
	Mount *m;

	qlock(&shrslk);
	for(shr = shrs; shr != nil; shr = shr->next){
		wlock(&shr->umh.lock);
		for(m = shr->umh.mount; m != nil; m = m->next)
			chowner(tompt(m), old, new);
		wunlock(&shr->umh.lock);
		chowner(shr, old, new);
	}
	qunlock(&shrslk);
}