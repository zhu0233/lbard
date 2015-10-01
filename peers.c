/*
Serval Low-bandwidth asychronous Rhizome Demonstrator.
Copyright (C) 2015 Serval Project Inc.

This program monitors a local Rhizome database and attempts
to synchronise it over low-bandwidth declarative transports, 
such as bluetooth name or wifi-direct service information
messages.  It is intended to give a high priority to MeshMS
converations among nearby nodes.

The design is fully asynchronous, so a call to the update_my_message()
function from time to time should be all that is required.


This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <assert.h>

#include "lbard.h"


int free_peer(struct peer_state *p)
{
  if (p->sid_prefix) free(p->sid_prefix); p->sid_prefix=NULL;
  for(int i=0;i<p->bundle_count;i++) {
    if (p->bid_prefixes[i]) free(p->bid_prefixes[i]);    
  }
  for(int i=0;i<4;i++) p->sid_prefix_bin[i]=0;
  free(p->bid_prefixes); p->bid_prefixes=NULL;
  free(p->versions); p->versions=NULL;
  free(p->size_bytes); p->size_bytes=NULL;
  free(p);
  return 0;
}

int peer_note_bar(struct peer_state *p,
		  char *bid_prefix,long long version, char *recipient_prefix,
		  int size_byte)
{
  int b=-1;

  if (0) {
    for(int i=0;i<p->bundle_count;i++)
      fprintf(stderr,"  bundle #%d/%d: %s* version %lld\n",
	      i,p->bundle_count,
	      p&&p->bid_prefixes&&p->bid_prefixes[i]?p->bid_prefixes[i]:"<bad>",
	      p&&p->versions&&p->versions[i]?p->versions[i]:-1);
    fprintf(stderr,"  bundle list end.\n");
  }
  
  // XXX Argh! Another linear search! Replace with something civilised
  for(int i=0;i<p->bundle_count;i++)
    if (!strcmp(p->bid_prefixes[i],bid_prefix)) {
      b=i;
      if (0) fprintf(stderr,"Peer %s* has bundle %s* version %lld (we already knew that they have version %lld)\n",
		     p->sid_prefix,bid_prefix,version,p->versions[b]);
      break;
    }
  if (b==-1) {
    // New bundle.
    if (0) fprintf(stderr,"Peer %s* has bundle %s* version %lld, which we haven't seen before\n",
	    p->sid_prefix,bid_prefix,version);
    if (p->bundle_count>=MAX_PEER_BUNDLES) {
      // BID list too full -- random replacement.
      b=random()%p->bundle_count;
      free(p->bid_prefixes[b]); p->bid_prefixes[b]=NULL;
    }
    if (p->bundle_count>=p->bundle_count_alloc) {
      // Allocate new list space
      p->bundle_count_alloc+=1000;
      p->bid_prefixes=realloc(p->bid_prefixes,sizeof(char *)*p->bundle_count_alloc);
      assert(p->bid_prefixes);
      p->versions=realloc(p->versions,sizeof(long long)*p->bundle_count_alloc);
      assert(p->versions);
      p->size_bytes=realloc(p->versions,p->bundle_count_alloc);
      assert(p->size_bytes);
    }
    b=p->bundle_count;
    if (1) fprintf(stderr,"Peer %s* bundle %s* will go in index %d (current count = %d)\n",
	    p->sid_prefix,bid_prefix,b,p->bundle_count);
    p->bid_prefixes[b]=strdup(bid_prefix);
    if (b>=p->bundle_count) p->bundle_count++;
  }
  assert(p);
  p->versions[b]=version;
  p->size_bytes[b]=size_byte;
  
  return 0;
}

struct peer_state *peer_records[MAX_PEERS];
int peer_count=0;

int find_peer_by_prefix(char *peer_prefix)
{
  for(int i=0;i<peer_count;i++)
    if (!strcasecmp(peer_records[i]->sid_prefix,peer_prefix))
      return i;
  
  return -1;
}

// The most interesting bundle a peer has is the smallest MeshMS bundle, if any, or
// else the smallest bundle that it has, but that we do not have.
// XXX - We should take into account neighbours we can see to prioritise content for
// them (and neighbours of neighbours, since phones are usually not going to be running
// lbard).
// XXX - Actually, we can't even take into account MeshMS or size, because we currenlty
// only store version and BID prefix. version will do as a decent proxy for these
// things for now, since for meshms version is the journal bundle length.
// XXX - We should also not ask for bundles we are currently transferring from other
// peers. This is tricky to enforce, because there are race conditions that could
// easily end up with this exact scenario.
int peers_most_interesting_bundle(int peer)
{
  int best_bundle=-1;
  int bundle;
  int i;
  // XXX - More linear searches!
  for (bundle=0;bundle<peer_records[peer]->bundle_count;bundle++) {
    int not_interesting=0;
    if (!peer_records[peer]->bid_prefixes[bundle]) not_interesting=1;
    else {
      for(i=0;i<bundle_count;i++) {
	if (!strncasecmp(bundles[i].bid,peer_records[peer]->bid_prefixes[bundle],
			 strlen(peer_records[peer]->bid_prefixes[bundle]))) {
	  // We have this bundle, but do we have this version?
	  if (bundles[i].version>=peer_records[peer]->versions[bundle]) {
	    // Ok, we have this already, so it is not interesting.
	    not_interesting=1;
	    break;
	  }
	}
      }
    }
    if (!not_interesting) {
      // Bundle is more interesting if it is smaller or meshms and the best so far is
      // not. We do this by comparing size_bytes first, which will put all meshms to
      // the front of the queue, and then version, which will allow finer
      // discrimination of size differences if size_bytes are identical.

      if ((best_bundle==-1)
	  ||(peer_records[peer]->size_bytes[bundle]<peer_records[peer]->size_bytes[best_bundle])
	  ||((peer_records[peer]->size_bytes[bundle]==peer_records[peer]->size_bytes[best_bundle])
	     &&(peer_records[peer]->versions[bundle]<peer_records[peer]->versions[best_bundle])))
	best_bundle=bundle;
    }
  }

  return best_bundle;
}

int hex_to_val(int c)
{
  if (c>='0'&&c<='9') return c-'0';
  if (c>='A'&&c<='F') return c-'A'+10;
  if (c>='a'&&c<='f') return c-'a'+10;
  return 0;
}

int request_segment(int peer, char *bid_prefix, int seg_start, int is_manifest,
		    int *offset, int mtu,unsigned char *msg_out)
{
  // Check that we have enough space
  if ((mtu-*offset)<(1+2+8+3)) return -1;

  int start_offset=*offset;
  
  // Request piece
  msg_out[(*offset)++]='R';

  // First 2 bytes only of peer SID. This will almost always get the benefit of
  // specifying the peer precisely, but save bytes on average
  msg_out[(*offset)++]=peer_records[peer]->sid_prefix_bin[0];
  msg_out[(*offset)++]=peer_records[peer]->sid_prefix_bin[1];

  // BID prefix
  for(int i=0;i<8;i++)
    msg_out[(*offset)++]=
      hex_to_val(bid_prefix[i*2+1])
      +hex_to_val(bid_prefix[i*2+0])*16;

  // Start offset of interest
  msg_out[(*offset)++]=(seg_start>>0)&0xff;
  msg_out[(*offset)++]=(seg_start>>8)&0xff;
  msg_out[(*offset)++]=((seg_start>>16)&0x7f)|(is_manifest?0x80:0x00);

  if (debug_pull) {
    fprintf(stderr,"Requesting BID=%s @ %c%d from SID=%s*\n",
	    bid_prefix,
	    is_manifest?'M':'B',seg_start,peer_records[peer]->sid_prefix);
    fprintf(stderr,"Request block: ");
    for(;start_offset<*offset;start_offset++)
      fprintf(stderr," %02X",msg_out[start_offset]);
    fprintf(stderr,"\n");
  }
  
  return 0;
}

int last_peer_requested=0;

int request_wanted_content_from_peers(int *offset,int mtu, unsigned char *msg_out)
{
  int peer;

  // Work out who to ask next?
  // (consider all peers in round-robin)
  if (last_peer_requested>=peer_count) last_peer_requested=0;
  peer=last_peer_requested;
  last_peer_requested++;
  
  for(;peer<peer_count;peer++)
    {
      // Keep track of what we are getting from this peer, and try to finish
      // the most complete things first.
      // XXX - It would really help if we knew in advance the length of a payload
      // so that we could actually do this properly.
      // XXX - Instead for now, we just request the first missing thing we have.
      // XXX - We now have access to size_bytes from BARs to help us prioritise
      // content.
      // int most_complete_partial=-1;
      // int most_complete_remaining=-1;
      // int most_complete_start=-1;
      // int most_complete_manifest_or_body=-1;
      
      // What are we fetching from this peer?
      int i;
      
      for(i=0;i<MAX_BUNDLES_IN_FLIGHT;i++) {
	if (peer_records[peer]->partials[i].bid_prefix) {
	  // We are receiving something from this peer, so we presume it is
	  // interesting.
	  // Our approach to requesting missing parts is simple:
	  // 1. Request missing stuff from the start, if any.
	  // 2. Else, request from the end of the first segment, so that we will tend
	  // to merge segments.
	  struct segment_list *s=peer_records[peer]->partials[i].manifest_segments;
	  if (s&&(s->start_offset||(s->length<peer_records[peer]->partials[i].manifest_length)||peer_records[peer]->partials[i].manifest_length<0))
	    {
	      if (debug_pull) {
		fprintf(stderr,"We need manifest bytes...\n");
		dump_segment_list(s);
	      }
	      if (s&&s->start_offset) {
		// We are missing bytes at the beginning
		return request_segment(peer,
				       peer_records[peer]->partials[i].bid_prefix,
				       0,1 /* manifest */,offset,mtu,msg_out);
	      } else if (s) {
		if (debug_pull) {
		  fprintf(stderr,"We need manifest bytes...\n");
		  dump_segment_list(s);
		}
		return request_segment(peer,
				       peer_records[peer]->partials[i].bid_prefix,
				       (s->start_offset+s->length),
				       1 /* manifest */,offset,mtu,msg_out);
	      }
	    }
	  s=peer_records[peer]->partials[i].body_segments;
	  if (s&&s->start_offset) {
	    // We are missing bytes at the beginning
	    if (debug_pull) {
	      fprintf(stderr,"We need body bytes at the start (start_offset=%d)...\n",s->start_offset);
	      dump_segment_list(s);
	    }
	    return request_segment(peer,
				   peer_records[peer]->partials[i].bid_prefix,
				   0,
				   0 /* not manifest */,offset,mtu,msg_out);
	  } else if (s) {
	    if (debug_pull) {
	      fprintf(stderr,"We need body bytes @ %d...\n",
				    s->start_offset+s->length);
	      dump_segment_list(s);
	    }
	    return request_segment(peer,
				   peer_records[peer]->partials[i].bid_prefix,
				   (s->start_offset+s->length),
				   0 /* not manifest */,offset,mtu,msg_out);
	  }
	}
      }
      // If we got here, the peer is not currently sending us anything interesting.
      // So have a look at what the peer has to offer, and ask for something
      // interesting.
      int bundle=peers_most_interesting_bundle(peer);
      if (bundle>-1) {
	if (debug_pull) fprintf(stderr,"We need an interesting bundle...\n");
	return request_segment(peer,peer_records[peer]->bid_prefixes[bundle],
			       0,0 /* not manifest */,offset,mtu,msg_out);
      }
    }
  return 0;
}
