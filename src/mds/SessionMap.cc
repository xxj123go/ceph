// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include "MDS.h"
#include "MDCache.h"
#include "SessionMap.h"
#include "osdc/Filer.h"

#include "config.h"

#define DOUT_SUBSYS mds
#undef dout_prefix
#define dout_prefix *_dout << dbeginl << "mds" << mds->get_nodeid() << ".sessionmap "


void SessionMap::dump()
{
  hash<entity_name_t> H;
  dout(0) << "dump" << dendl;
  for (hash_map<entity_name_t,Session*>::iterator p = session_map.begin();
       p != session_map.end();
       ++p) 
    dout(0) << p->first << " " << p->second << " hash " << H(p->first) << " addr " << (void*)&p->first << dendl;
}


// ----------------
// LOAD


object_t SessionMap::get_object_name()
{
  char s[30];
  snprintf(s, sizeof(s), "mds%d_sessionmap", mds->whoami);
  return object_t(s);
}

class C_SM_Load : public Context {
  SessionMap *sessionmap;
public:
  bufferlist bl;
  C_SM_Load(SessionMap *cm) : sessionmap(cm) {}
  void finish(int r) {
    sessionmap->_load_finish(r, bl);
  }
};

void SessionMap::load(Context *onload)
{
  dout(10) << "load" << dendl;

  if (onload)
    waiting_for_load.push_back(onload);
  
  C_SM_Load *c = new C_SM_Load(this);
  object_t oid = get_object_name();
  OSDMap *osdmap = mds->objecter->osdmap;
  ceph_object_layout ol = osdmap->make_object_layout(oid,
						     mds->mdsmap->get_metadata_pg_pool());
  mds->objecter->read_full(oid, ol, CEPH_NOSNAP, &c->bl, 0, c);
}

void SessionMap::_load_finish(int r, bufferlist &bl)
{ 
  bufferlist::iterator blp = bl.begin();
  decode(blp);  // note: this sets last_cap_renew = now()
  dout(10) << "_load_finish v " << version 
	   << ", " << session_map.size() << " sessions, "
	   << bl.length() << " bytes"
	   << dendl;
  projected = committing = committed = version;
  dump();
  finish_contexts(waiting_for_load);
}


// ----------------
// SAVE

class C_SM_Save : public Context {
  SessionMap *sessionmap;
  version_t version;
public:
  C_SM_Save(SessionMap *cm, version_t v) : sessionmap(cm), version(v) {}
  void finish(int r) {
	sessionmap->_save_finish(version);
  }
};

void SessionMap::save(Context *onsave, version_t needv)
{
  dout(10) << "save needv " << needv << ", v " << version << dendl;
 
  if (needv && committing >= needv) {
    assert(committing > committed);
    commit_waiters[committing].push_back(onsave);
    return;
  }

  commit_waiters[version].push_back(onsave);
  
  bufferlist bl;
  
  encode(bl);
  committing = version;
  SnapContext snapc;
  object_t oid = get_object_name();
  OSDMap *osdmap = mds->objecter->osdmap;
  ceph_object_layout ol = osdmap->make_object_layout(oid,
						     mds->mdsmap->get_metadata_pg_pool());

  mds->objecter->write_full(oid, ol,
			    snapc,
			    bl, g_clock.now(), 0,
			    NULL, new C_SM_Save(this, version));
}

void SessionMap::_save_finish(version_t v)
{
  dout(10) << "_save_finish v" << v << dendl;
  committed = v;

  finish_contexts(commit_waiters[v]);
  commit_waiters.erase(v);
}


// -------------------

void SessionMap::encode(bufferlist& bl)
{
  ::encode(version, bl);

  // this is a meaningless upper bound, because we don't include all
  // sessions below.  it can be ignored by decode().
  __u32 n = session_map.size();
  ::encode(n, bl);

  for (hash_map<entity_name_t,Session*>::iterator p = session_map.begin(); 
       p != session_map.end(); 
       ++p) 
    if (p->second->is_open() ||
	p->second->is_closing() ||
	p->second->is_stale() ||
	p->second->is_stale_purging() ||
	p->second->is_stale_closing())
      p->second->encode(bl);
}

void SessionMap::decode(bufferlist::iterator& p)
{
  utime_t now = g_clock.now();

  ::decode(version, p);

  // this is a meaningless upper bound.  can be ignored.
  __u32 n;
  ::decode(n, p);

  while (n-- && !p.end()) {
    Session *s = new Session;
    s->decode(p);
    session_map[s->inst.name] = s;
    set_state(s, Session::STATE_OPEN);
    s->last_cap_renew = now;
  }
}
