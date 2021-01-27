

// Makes an MGEN text log file from a pcap trace file
// (the pcap log time is used as MGEN "recv" time)
// Assumes UDP packets in tcpdump trace file (pcap file) are
// MGEN packets and parses to build an MGEN log file

// Notes on options:
//
// 1) The "trace" option prepends MGEN log lines with epoch time and MAC src/addr information


#include <string.h>
#include <stdio.h>
#include <iostream>
#include <pcap.h>
#include "protoPktETH.h" // for Ethernet frame parsing
#include "protoPktIP.h"  // for IP packet parsing
#include "protoPktARP.h"
#include "mgenMsg.h"
#include "protoTime.h"
#include "mgen.h"

#include "influxdb.h"

#define MAX_LINE_SIZE (8000)

#define INFLUX_DB_HOST "127.0.0.1"
#define INFLUX_DB_PORT 8089

#define tv2dbl(tv) ((tv).tv_sec + (tv).tv_usec / 1000000.0)

influx_client_t client_info = {
    .host = strdup(INFLUX_DB_HOST),
    .port = INFLUX_DB_PORT,
    .db = NULL,
    .usr = NULL,
    .pwd = NULL
};

influx_client_t* pClient_info = &client_info;

enum CmdType {CMD_INVALID, CMD_ARG, CMD_NOARG};
// default arg values. Global for now--TODO; make this more C++ code (i.e., pcap2mgen class) and less C code
bool compute_analytics = false;
double analytic_window = MgenAnalytic::DEFAULT_WINDOW;
MgenAnalyticTable analytic_table;
// MgenFlowList flow_list;
// unsigned int default_flow_label = 0;
// int default_queue_limit = 0;
FILE* infile = stdin;
FILE* outfile = stdout;
bool trace = false;
bool log_rx = true;

bool flush = false;


const char* const CMD_LIST[] =
{
    "-report",    // Grab MGEN REPORT messages from pcap file
    "-analytic",  // Grab MGEN REPORT messages from pcap file
    "+infile",    // Name of pcap input file
    "+outfile",   // Name of output file
    "-trace",     // Prepends MGEN log lines with epoch time and MAC src/addr info
    "+rxlog",     // Turns on/off recv log info. For report messages only
    "-flush",     // flush writes to outfile
    "+window",    // Sets analytic window
    "+scenario",  // The name of the scenario
    "+uuid",      // The UUID of the scenario
    NULL
};

char* scenario_name = strdup("NA");
char* scenario_uuid = strdup("NA");

void Usage()
{
    fprintf(stderr, "pcap2mgen [trace][epoch][pcapInputFile [mgenOutputFile]]\n");
}

void SetAnalyticWindow(double windowSize)
{
    if (windowSize <= 0.0) return;
    unsigned short int q = MgenAnalytic::Report::QuantizeTimeValue(windowSize);
    analytic_window = MgenAnalytic::Report::UnquantizeTimeValue(q);
    MgenAnalyticTable::Iterator iterator(analytic_table);
    MgenAnalytic* next;
    while (NULL != (next = iterator.GetNextItem()))
        next->SetWindowSize(windowSize);
}


CmdType GetCmdType(const char* cmd)
{
    if (!cmd) return CMD_INVALID;
    unsigned int len = strlen(cmd);

    bool matched = false;
    CmdType type = CMD_INVALID;
    const char* const* nextCmd = CMD_LIST;
    while (*nextCmd)
    {
        char lowerCmd[32];  // all commands < 32 characters
        len = len < 31 ? len : 31;
        unsigned int i;

        for (i = 0; i < (len + 1); i++)
        {
            lowerCmd[i] = tolower(cmd[i]);
        }

        if (!strncmp(lowerCmd, *nextCmd + 1, len))
        {
            if (matched)
            {
                // ambiguous command, should only match oncee
                return CMD_INVALID;
            }
            else
            {
                matched = true;
                if ('+' == *nextCmd[0])
                    type = CMD_ARG;
                else
                    type = CMD_NOARG;
            }
        }
        nextCmd++;
    }
    return type;
}   // end GetCmdType


bool OnCommand(const char* cmd, const char* val)
{
    CmdType type = GetCmdType(cmd);
    unsigned int len = strlen(cmd);
    char lowerCmd[32];  // all commands < 32 characters
    len = len < 31 ? len : 31;
    unsigned int i;
    for (i = 0; i < (len + 1); i++)
        lowerCmd[i] = tolower(cmd[i]);

    if (CMD_INVALID == type)
    {
        fprintf(stderr, "pcap2mgen ProcessCommands(%s) error: invalid command\n", cmd);
        return false;
    }
    else if ((CMD_ARG == type) && (NULL == val))
    {
        fprintf(stderr, "pcap2mgen ProcessCommands(%s) missing argument\n", cmd);
        return false;
    }
    else if (!strncmp("analytic", lowerCmd, len))
    {
        compute_analytics = true;
    }

    else if (!strncmp("report", lowerCmd, len))
    {
        compute_analytics = true;
    }
    else if (!strncmp("infile", lowerCmd, len))
    {
        if(NULL == (infile = fopen(val, "r")))
        {
            fprintf(stderr, "pcap2mgen: error opening input file: %s", val);
            return false;
        }
    }
    else if (!strncmp("outfile", lowerCmd, len))
    {
        if(NULL == (outfile = fopen(val, "w+")))
        {
            fprintf(stderr, "pcap2mgen: error opening output file: %s", val);
            return false;
        }
    }
    else if (!strncmp("trace", lowerCmd, len))
    {
        trace = true;
    }
    else if (!strncmp("flush", lowerCmd, len))
    {
        flush = true;
    }
    else if (!strncmp("scenario", lowerCmd, len))
    {
        if(NULL != val)
        {
            free(scenario_name);
            scenario_name = strdup(val);
        }
        else
        {
            fprintf(stderr, "pcap2mgen OnCommand() Error: missing argument to scenario\n");
        }
    }
    else if (!strncmp("uuid", lowerCmd, len))
    {
        if(NULL != val)
        {
            free(scenario_uuid);
            scenario_uuid = strdup(val);
        }
        else
        {
            fprintf(stderr, "pcap2mgen OnCommand() Error: missing argument to uuid\n");
        }
    }
    else if (!strncmp("rxlog", lowerCmd, len))
    {
        // std::cout << "lowerCmd = " << lowerCmd << std::endl;
        if(!val)
        {
            fprintf(stderr, "pcap2mgen OnCommand() Error: missing argument to rxlog\n");
            return false;
        }
        bool rxLogTmp;
        // convert to lower case for case-insensitivity
        char temp[4];
        unsigned int len = strlen(val);
        len = len < 4 ? len : 4;
        unsigned int j;
        for(j = 0; j < len; j++)
            temp[j] = tolower(val[j]);
        temp[j] = '\0';
        if(!strncmp("on", temp, len))
            rxLogTmp = true;
        else if(!strncmp("off", temp, len))
            rxLogTmp = false;
        else
        {
            fprintf(stderr, "pcap2mgen OnCommand Error: wrong argument to rxlog: %s\n", val);
            return false;
        }
        log_rx = rxLogTmp;
    }
    else if (!strncmp("window", lowerCmd, len))
    {
        if (NULL != val)
        {
            double windowSize;
            if((1 != sscanf(val, "%lf", &windowSize)) || (windowSize <= 0.0))
            {
                fprintf(stderr, "Mgen::OnCommand() Error: invalid WINDOW interval\n");
            }
            SetAnalyticWindow(windowSize);
        }
        else
        {
            fprintf(stderr, "Mgen::OnCommand() Error: missing argument to WINDOW\n");
            return false;
        }
    }

    return true;
}


bool ProcessCommands(int argc, const char*const* argv)
{
    int i = 1;
    // bool convert = false;

    while (i < argc)
    {
        CmdType cmdType = GetCmdType(argv[i]);

        switch (cmdType)
        {
            case CMD_INVALID:
                fprintf(stderr, "pcap2mgen error: invalid command: %s\n", argv[i]);
                return false;
            case CMD_NOARG:
                if(!OnCommand(argv[i], NULL))
                {
                    fprintf(stderr, "pcap2mgen OnCommand(%s) error\n", argv[i]);
                    return false;
                }
                i++;
                break;
            case CMD_ARG:
                if(!OnCommand(argv[i], argv[i+1]))
                {
                    fprintf(stderr, "pcap2mgen OnCommand(%s, %s) error\n", argv[i], argv[i+1]);
                    return false;
                }
                i += 2;
                break;
        }
    }
    return true;
}  // end ProcessCommands()



int main(int argc, char* argv[])
{
    // bool debug = true;
    if(!ProcessCommands(argc, argv))
    {
        fprintf(stderr, "pcap2mgen: error while processing startup commands\n");
    }
    // bool trace = false;
    // // First pull out any "options" from command line
    // int argOffset = 0;
    // for (int i = 1; i < argc; i++)
    // {
    //     if (0 == strcmp("trace", argv[i]))
    //     {
    //         trace = true;  // enables prepending of MAC src/dst addr info
    //     }
    //     else
    //     {
    //         // It's not an option so assume it's a file name
    //         argOffset = i - 1;
    //         break;
    //     }
    // }

    fprintf(stderr,"Pcap Version: %s\n",pcap_lib_version());

    // if(debug)
    // {
    //     std::cout << "log_rx = " << log_rx << std::endl;
    //     std::cout << "compute_analytics = " << compute_analytics << std::endl;
    // }


    // Use stdin/stdout by default

    // switch(argc - argOffset)
    // {
    //     case 1:
    //         // using default stdin/stdout
    //         break;
    //     case 2:
    //         // using named input pcap file and stdout
    //         if (NULL == (infile = fopen(argv[1 + argOffset], "r")))
    //         {
    //             perror("pcap2mgen: error opening input file");
    //             return -1;
    //         }
    //         break;
    //     case 3:
    //         // use name input and output files
    //         if (NULL == (infile = fopen(argv[1 + argOffset], "r")))
    //         {
    //             perror("pcap2mgen: error opening input file");
    //             return -1;
    //         }
    //         if (NULL == (outfile = fopen(argv[2 + argOffset], "w+")))
    //         {
    //             perror("pcap2mgen: error opening output file");
    //             return -1;
    //         }
    //         break;
    //     default:
    //         fprintf(stderr, "pcap2mgen: error: too many arguments!\n");
    //         Usage();
    //         return -1;
    // }  // end switch(argc)

    // flow_list.SetDefaultLabel(default_flow_label);

    char pcapErrBuf[PCAP_ERRBUF_SIZE+1];
    pcapErrBuf[PCAP_ERRBUF_SIZE] = '\0';
    pcap_t* pcapDevice = pcap_fopen_offline(infile, pcapErrBuf);
    if (NULL == pcapDevice)
    {
        fprintf(stderr, "pcap2mgen: pcap_fopen_offline() error: %s\n", pcapErrBuf);
        if (stdin != infile) fclose(infile);
        if (stdout != outfile) fclose(outfile);
        return -1;
    }

    int linkType = pcap_datalink(pcapDevice);




    UINT32 alignedBuffer[4096/4];   // 128 buffer for packet parsing
    memset(alignedBuffer, 0, 4096);
    UINT16* ethBuffer = ((UINT16*)alignedBuffer) + 1;
    unsigned int maxBytes = 4096 - 2;  // due to offset, can only use 4094 bytes of buffer

    pcap_pkthdr hdr;
    const u_char* pktData;
    char dst_addr[16]   = {0};
    char dst_port[6]    = {0};
    char src_addr[16]   = {0};
    char src_port[6]    = {0};
    char msg_len[6]     = {0};
    char flow_str[11]   = {0};

    struct timeval delta_time   = {0};
    struct timeval start_time   = {0};
    struct timeval end_time     = {0};
    struct timeval tx_time      = {0};
    size_t report_count         =  0;
    size_t pkt_count            =  0;
    char* line                  = (char*)calloc(MAX_LINE_SIZE + 1000, sizeof(uint8_t));
    int used                    =  0;
    volatile int len            =  MAX_LINE_SIZE;
    ProtoAddress addr = ProtoAddress();
    while(NULL != (pktData = pcap_next(pcapDevice, &hdr)))
    {
        unsigned int numBytes = maxBytes;
        if (hdr.caplen < numBytes) numBytes = hdr.caplen;

        ProtoPktETH::Type ethType;
        ProtoAddress srcMac, dstMac;
        unsigned int ipLength;  // will be IP packet payload size (incl. IP header)
        void* ipBuffer;
        unsigned ipBufferBytes;
        if (DLT_LINUX_SLL == linkType)
        {
            // For now, assume the header is 16 bytes (6-byte link addr)
            // TBD - do proper DLT_LINUX_SLL parsing
            memcpy(alignedBuffer, pktData, numBytes);
            ethType = (ProtoPktETH::Type)ntohs(((UINT16*)alignedBuffer)[7]);
            ipBuffer = alignedBuffer + 4;  // assumes 16 byte header
            ipBufferBytes = maxBytes + 2 - 16;
            ipLength = numBytes - 16;

        }
        else
        {
            // Should we verify this is DLT_EN10MB?
            memcpy(ethBuffer, pktData, numBytes);
            ProtoPktETH ethPkt(ethBuffer, maxBytes);
            if (!ethPkt.InitFromBuffer(hdr.len))
            {
                fprintf(stderr, "pcap2mgen error: invalid Ether frame in pcap file\n");
                continue;
            }
            ethType = ethPkt.GetType();
            ipBuffer = ethPkt.AccessPayload();
            ipBufferBytes = ethPkt.GetBufferLength() - ethPkt.GetHeaderLength();
            ipLength = ethPkt.GetPayloadLength();
            ethPkt.GetSrcAddr(srcMac);
            ethPkt.GetSrcAddr(dstMac);
        }
        ProtoPktIP ipPkt;
        ProtoAddress srcAddr, dstAddr;
        int ttl;
        if ((ProtoPktETH::IP == ethType) ||
            (ProtoPktETH::IPv6 == ethType))
        {
            if (!ipPkt.InitFromBuffer(ipLength, ipBuffer, ipBufferBytes))
            {
                fprintf(stderr, "pcap2mgen error: bad IP packet\n");
                continue;
            }
            switch (ipPkt.GetVersion())
            {
                case 4:
                {
                    ProtoPktIPv4 ip4Pkt(ipPkt);
                    ip4Pkt.GetDstAddr(dstAddr);
                    ip4Pkt.GetSrcAddr(srcAddr);
                    ttl = ip4Pkt.GetTTL();
                    break;
                }
                case 6:
                {
                    ProtoPktIPv6 ip6Pkt(ipPkt);
                    ip6Pkt.GetDstAddr(dstAddr);
                    ip6Pkt.GetSrcAddr(srcAddr);
                    ttl = ip6Pkt.GetHopLimit();
                    break;
                }
                default:
                {
                    ttl = -1;
                    PLOG(PL_ERROR,"pcap2mgen Error: Invalid IP pkt version.\n");
                    break;
                }
            }
            //TRACE("pcap2mgen IP packet dst>%s ", dstAddr.GetHostString());
            //TRACE(" src>%s length>%d\n", srcAddr.GetHostString(), ipPkt.GetLength());
        }


        if (!srcAddr.IsValid()) continue;  // wasn't an IP packet

        ProtoPktUDP udpPkt;
	        if (!udpPkt.InitFromPacket(ipPkt)) continue;  // not a UDP packet

        MgenMsg msg;
        if (!msg.Unpack((UINT32*)udpPkt.AccessPayload(), udpPkt.GetPayloadLength(), false, false))
        {
            fprintf(stderr, "pcap2mgen warning: UDP packet not an MGEN packet?\n");
            continue;
        }
        msg.SetProtocol(UDP);
        srcAddr.SetPort(udpPkt.GetSrcPort());
        msg.SetSrcAddr(srcAddr);

        if (trace && (DLT_LINUX_SLL != linkType))
        {
            fprintf(outfile, "%lu.%lu ", (unsigned long)hdr.ts.tv_sec, (unsigned long)hdr.ts.tv_usec);
            fprintf(outfile, "esrc>%s ", srcMac.GetHostString());
            fprintf(outfile, "edst>%s ", dstMac.GetHostString());
        }
        // TBD - Add option to log REPORT events only?  Embed MGEN analytic, too?
        if(compute_analytics)
        {
            MgenAnalytic* analytic = analytic_table.FindFlow(msg.GetSrcAddr(), msg.GetDstAddr(), msg.GetFlowId());
            if (NULL == analytic)
            {
                if (NULL == (analytic = new MgenAnalytic()))
                {
                    fprintf(stderr, "Mgen::UpdateRecvAnalytics() new MgenAnalytic() error: %s\n", GetErrorString());
                    return -1;
                }
                if (!analytic->Init(msg.GetProtocol(), msg.GetSrcAddr(), msg.GetDstAddr(), msg.GetFlowId(), analytic_window))
                {
                    fprintf(stderr, "Mgen::UpdateRecvAnalytics() MgenAnalytic() initialization error: %s\n", GetErrorString());
                    return -1;
                }
                if (!analytic_table.Insert(*analytic))
                {
                    fprintf(stderr, "Mgen::UpdateRecvAnalytics() unable to add new flow analytic: %s\n", GetErrorString());
                    delete analytic;
                    return -1;
                }
            }

            ProtoTime rxTime(hdr.ts);
            if (analytic->Update(rxTime, msg.GetMsgLen(), ProtoTime(msg.GetTxTime()), msg.GetSeqNum()))
            {
                const MgenAnalytic::Report& report = analytic->GetReport(rxTime);

                report.GetDstAddr(addr);
                inet_ntop(AF_INET, addr.GetRawHostAddress(), dst_addr, sizeof(dst_addr));
                snprintf(dst_port, sizeof(dst_port), "%hu", addr.GetPort());
                report.GetSrcAddr(addr);

                inet_ntop(AF_INET, addr.GetRawHostAddress(), src_addr, sizeof(src_addr));
                snprintf(src_port, sizeof(src_port), "%hu", addr.GetPort());
                bzero(flow_str, sizeof(flow_str));
                snprintf(flow_str, sizeof(flow_str), "%u", report.GetFlowId());
                puts(flow_str);
                used = format_line(&line, &len, used,
                    INFLUX_MEAS("mgen_report_post_test"),
                    INFLUX_TAG("dst_addr", dst_addr),
                    INFLUX_TAG("dst_port", dst_port),
                    INFLUX_TAG("flow", flow_str),
                    INFLUX_TAG("proto", "udp"),
                    INFLUX_TAG("scenario", scenario_name),
                    INFLUX_TAG("src_addr", src_addr),
                    INFLUX_TAG("src_port", src_port),
                    INFLUX_TAG("uuid", scenario_uuid),
                    INFLUX_F_FLT("loss", report.GetLossFraction(), 6),
                    INFLUX_F_FLT("rate", report.GetRateAve(), 4),
                    INFLUX_F_FLT("window", report.GetWindowSize(), 6),
                    INFLUX_F_FLT("min_latency", report.GetLatencyMin(), 6),
                    INFLUX_F_FLT("max_latency", report.GetLatencyMax(), 6),
                    INFLUX_F_FLT("avg_latency", report.GetLatencyAve(), 6),
                    INFLUX_TS(((((hdr.ts.tv_sec)*1000000) + hdr.ts.tv_usec) * 1000)),
                    INFLUX_END
                );
                ++report_count;
                // report.Log(outfile, rxTime, rxTime, false);
                if(MAX_LINE_SIZE <= used) {
                    send_udp_line(pClient_info, line, used);
                    used = 0;
                }

            }
            // we could dalso keep the analytic in a list and prune stale ones
        }
        // inet_ntop(AF_INET, msg.GetDstAddr().GetRawHostAddress(), dst_addr, sizeof(dst_addr));
        // snprintf(dst_port, sizeof(dst_port), "%hu", msg.GetDstAddr().GetPort());
        // inet_ntop(AF_INET, msg.GetSrcAddr().GetRawHostAddress(), src_addr, sizeof(src_addr));
        // snprintf(src_port, sizeof(src_port), "%hu", msg.GetSrcAddr().GetPort());
        // snprintf(msg_len, sizeof(msg_len), "%hu", msg.GetMsgLen());
        // snprintf(flow_str, sizeof(flow_str), "%u", msg.GetFlowId());
        // tx_time = (msg.GetTxTime());
        // ProtoTime rxTime(hdr.ts);
        // ProtoTime txTime(tx_time);
        // // send_udp(pClient_info,
        // used = format_line(&line, &len, used,
        //     INFLUX_MEAS("mgen_recv_test"),
        //     INFLUX_TAG("dst_addr", dst_addr),
        //     INFLUX_TAG("dst_port", dst_port),
        //     INFLUX_TAG("flow", flow_str),
        //     INFLUX_TAG("proto", "udp"),
        //     INFLUX_TAG("scenario", scenario_name),
        //     INFLUX_TAG("src_addr", src_addr),
        //     INFLUX_TAG("src_port", src_port),
        //     INFLUX_TAG("uuid", scenario_uuid),
        //     INFLUX_F_FLT("delay", rxTime.GetValue() - txTime.GetValue(), 6),
        //     INFLUX_F_STR("gps", ""),
        //     INFLUX_F_INT("sent", ((((tx_time.tv_sec)*1000000) + tx_time.tv_usec)* 1000)),
        //     INFLUX_F_INT("seq", msg.GetSeqNum()),
        //     INFLUX_F_INT("size", msg.GetMsgLen()),
        //     INFLUX_TS(((((hdr.ts.tv_sec)*1000000) + hdr.ts.tv_usec) * 1000) + (pkt_count & 0x0000000000001FFF)),
        //     INFLUX_END
        // );
        ++pkt_count;
        // if(MAX_LINE_SIZE - 1000 <= used) {
        //     send_udp_line(pClient_info, line, used);
        //     used = 0;
        // }
        // msg.LogRecvEvent(outfile, false, false, log_rx, false, true, (UINT32*)udpPkt.AccessPayload(), flush, ttl, hdr.ts);  
    }  // end while (pcap_next())

    printf("Total reports generated: %lu\n", report_count);
    printf("Total reports generated: %lu\n", pkt_count);

    if(used > 0) {
        send_udp_line(pClient_info, line, used);
    }
    if (stdin != infile) fclose(infile);
    if (stdout != outfile) fclose(outfile);
    free(line);
    free(scenario_name);
    free(scenario_uuid);
    return 0;
}  // end main()
