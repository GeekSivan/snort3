/*
** Copyright (C) 2014 Cisco and/or its affiliates. All rights reserved.
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License Version 2 as
** published by the Free Software Foundation.  You may not use, modify or
** distribute this program under any other version of the GNU General
** Public License.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/
// packet_manager.cc author Josh Rosenbaum <jorosenba@cisco.com>

#include <list>
#include <vector>
#include <cstring>
#include "packet_manager.h"
#include "framework/codec.h"
#include "snort.h"
#include "thread.h"
#include "log/messages.h"
#include "packet_io/sfdaq.h"

#include "protocols/packet.h"
#include "protocols/undefined_protocols.h"
#include "time/profiler.h"


namespace
{
struct CdGenPegs{
    PegCount total_processed = 0;
    PegCount other_codecs = 0;
    PegCount discards = 0;
};

std::vector<const char*> CdGenPegNames =
{
    "total",
    "other",
    "discards"
};

} // anonymous


#ifdef PERF_PROFILING
THREAD_LOCAL PreprocStats decodePerfStats;
#endif

static const uint16_t max_protocol_id = 65535;
static std::array<Codec*, max_protocol_id> s_protocols;
static std::list<const CodecApi*> s_codecs;
static THREAD_LOCAL CdGenPegs pkt_cnt;
static CdGenPegs gpkt_cnt;


//-------------------------------------------------------------------------
// plugins
//-------------------------------------------------------------------------


/*
 *  THIS NEEDS TO GO INTO A README!!
 *
 *  PROTOCOL ID'S By Range
 *   0    (0x0000) -   255  (0x00FF)  --> Ip protocols
 *   256  (0x0100) -  1535  (0x05FF)  -->  random protocols (teredo, gtp)
 *  1536  (0x6000) -  65536 (0xFFFF)  --> Ethertypes
 */
void PacketManager::add_plugin(const CodecApi* api)
{
    if (!api->ctor)
        FatalError("Codec %s: ctor() must be implemented.  Look at the example code for an example.\n",
                        api->base.name);      

    if (!api->dtor)
        FatalError("Codec %s: dtor() must be implemented.  Look at the example code for an example.\n",
                        api->base.name);  

    s_codecs.push_back(api);
}

void PacketManager::release_plugins()
{
    for ( auto* p : s_codecs )
    {
        if(p->gterm)
            p->gterm();
        if(p->tterm)
            p->tterm();
//        p->dtor();
    }
    s_codecs.clear();
}

void PacketManager::dump_plugins()
{
    Dumper d("Codecs");

    for ( auto* p : s_codecs )
        d.dump(p->base.name, p->base.version);
}

//-------------------------------------------------------------------------
// grinder
//-------------------------------------------------------------------------

void PacketManager::decode(
    Packet* p, const DAQ_PktHdr_t* pkthdr, const uint8_t* pkt)
{
    PROFILE_VARS;
    int curr_prot_id, next_prot_id;
    uint16_t len, lyr_len;

    PREPROC_PROFILE_START(decodePerfStats);

    // initialize all of the relevent data to decode this packet
    memset(p, 0, PKT_ZERO_LEN);
    p->pkth = pkthdr;
    p->pkt = pkt;
    len = pkthdr->caplen;
    curr_prot_id = GRINDER_ID;
    pkt_cnt.total_processed++;

    // loop until the protocol id is no longer valid
    while(curr_prot_id  >= 0 && curr_prot_id < max_protocol_id)
    {
        if (s_protocols[curr_prot_id] == 0)
        {
            pkt_cnt.other_codecs++;
            break;
        }
        else if( !s_protocols[curr_prot_id]->decode(pkt, len, p, lyr_len, next_prot_id))
        {
            pkt_cnt.discards++;
            break;
        }           

        PacketClass::PushLayer(p, s_protocols[curr_prot_id], pkt, lyr_len);
        curr_prot_id = next_prot_id;
        next_prot_id = -1;
        len -= lyr_len;
        pkt += lyr_len;
        lyr_len = 0;
    }

    p->dsize = len;
    p->data = pkt;
    PREPROC_PROFILE_END(decodePerfStats);
}


void PacketManager::set_grinder(void)
{
    std::vector<uint16_t> proto;
    std::vector<int> dlt;
    bool codec_registered;

    int daq_dlt = DAQ_GetBaseProtocol();

    for ( auto* p : s_codecs )
    {
        codec_registered = false;

        if (p->ginit)
            p->ginit();

        if (p->tinit)
            p->tinit();

        // TODO:  add module
        // null check performed when plugin added.
        Codec *cd = p->ctor();


        proto.clear();
        if(p->proto_id)
            p->proto_id(proto);
        for (auto proto_id : proto)
        {
            if(s_protocols[proto_id] != NULL)
                WarningMessage("The Codecs %s and %s have both been registered "
                    "for protocol_id %d. Codec %s will be used\n",
                    s_protocols[proto_id]->get_name(), cd->get_name(), 
                    proto_id, cd->get_name());
            s_protocols[proto_id] = cd;
            codec_registered = true;
        }
        // add protocols to the array


        dlt.clear();
        if(p->dlt)
            p->dlt(dlt);
        // set the grinder if the data link types match
        for (auto curr_dlt : dlt )
        {
           if (curr_dlt == daq_dlt)
           {
                if (s_protocols[GRINDER_ID] != NULL)
                    WarningMessage("The Codecs %s and %s have both been registered "
                        "as the raw decoder. Codec %s will be used\n",
                        s_protocols[GRINDER_ID]->get_name(), cd->get_name(), 
                        cd->get_name());

                s_protocols[GRINDER_ID] = cd;
                codec_registered = true;
            }
        }

        if (!codec_registered)
            WarningMessage("The Codec %s is never used\n", cd->get_name());
    }
}

void PacketManager::dump_stats()
{
//    show_percent_stats(&pegs[0], &pegNames[0], pegNames.size(),
//        "codecs");
}

bool PacketManager::has_codec(uint16_t cd_id)
{
    return s_protocols[cd_id] != 0;
}

