
#include "VRClientMT.h"
#include "VRCManager.h"
#include "WorkTracer.h"
#include "FileHandler.h"

#ifndef USE_ODBC
#include "DBHandler.h"
#else
#include "DBHandler_ODBC.h"
#endif

#include "HAManager.h"
#include "stas.h"

#include <thread>
#include <iostream>
#include <fstream>

#include <string.h>

#include <boost/algorithm/string/replace.hpp>

// For Gearman
#include <libgearman/gearman.h>

#ifdef FAD_FUNC

#include <stdio.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <unistd.h>
#include <sys/mman.h>

#include <string.h>
#include <stdlib.h>

#include <fvad.h>

#ifdef USE_XREDIS
#include <iconv.h>

#include "rapidjson/document.h"     // rapidjson's DOM-style API
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#endif

#define WAVE_FORMAT_UNKNOWN      0X0000;
#define WAVE_FORMAT_PCM          0X0001;
#define WAVE_FORMAT_MS_ADPCM     0X0002;
#define WAVE_FORMAT_IEEE_FLOAT   0X0003;
#define WAVE_FORMAT_ALAW         0X0006;
#define WAVE_FORMAT_MULAW        0X0007;
#define WAVE_FORMAT_IMA_ADPCM    0X0011;
#define WAVE_FORMAT_YAMAHA_ADPCM 0X0016;
#define WAVE_FORMAT_GSM          0X0031;
#define WAVE_FORMAT_ITU_ADPCM    0X0040;
#define WAVE_FORMAT_MPEG         0X0050;
#define WAVE_FORMAT_EXTENSIBLE   0XFFFE;



typedef struct
{
	unsigned char ChunkID[4];    // Contains the letters "RIFF" in ASCII form
	unsigned int ChunkSize;      // This is the size of the rest of the chunk following this number
	unsigned char Format[4];     // Contains the letters "WAVE" in ASCII form
} RIFF;

//-------------------------------------------
// [Channel]
// - streo     : [left][right]
// - 3 channel : [left][right][center]
// - quad      : [front left][front right][rear left][reat right]
// - 4 channel : [left][center][right][surround]
// - 6 channel : [left center][left][center][right center][right][surround]
//-------------------------------------------
typedef struct
{
	unsigned char  ChunkID[4];    // Contains the letters "fmt " in ASCII form
	unsigned int   ChunkSize;     // 16 for PCM.  This is the size of the rest of the Subchunk which follows this number.
	unsigned short AudioFormat;   // PCM = 1
	unsigned short NumChannels;   // Mono = 1, Stereo = 2, etc.
	unsigned int   SampleRate;    // 8000, 44100, etc.
	unsigned int   AvgByteRate;   // SampleRate * NumChannels * BitsPerSample/8
	unsigned short BlockAlign;    // NumChannels * BitsPerSample/8
	unsigned short BitPerSample;  // 8 bits = 8, 16 bits = 16, etc
} FMT;


typedef struct
{
	char          ChunkID[4];    // Contains the letters "data" in ASCII form
	unsigned int  ChunkSize;     // NumSamples * NumChannels * BitsPerSample/8
} DATA;


typedef struct
{
	RIFF Riff;
	FMT	 Fmt;
	DATA Data;
} WAVE_HEADER;

#endif // FAD_FUNC

#ifdef USE_XREDIS
static unsigned int APHash(const char *str) {
    unsigned int hash = 0;
    int i;
    for (i=0; *str; i++) {
        if ((i&  1) == 0) {
            hash ^= ((hash << 7) ^ (*str++) ^ (hash >> 3));
        } else {
            hash ^= (~((hash << 11) ^ (*str++) ^ (hash >> 5)));
        }
    }
    return (hash&  0x7FFFFFFF);
}

enum {
 CACHE_TYPE_1, 
 CACHE_TYPE_2,
 CACHE_TYPE_MAX,
};
#endif

VRClientMT::VRClientMT(VRCManager* mgr, string& gearHost, uint16_t gearPort, int gearTimeout, string& fname, string& callid, string& counselcode, uint8_t jobType, uint8_t noc, FileHandler *deliver, DBHandler* s2d, bool is_save_pcm, string pcm_path, size_t framelen)
	: m_sGearHost(gearHost), m_nGearPort(gearPort), m_nGearTimeout(gearTimeout), m_sFname(fname), m_sCallId(callid), m_sCounselCode(counselcode), m_nLiveFlag(1), m_cJobType(jobType), m_nNumofChannel(noc), m_deliver(deliver), m_s2d(s2d), m_is_save_pcm(is_save_pcm), m_pcm_path(pcm_path), m_framelen(framelen*8)
{
	m_Mgr = mgr;
	m_thrd = std::thread(VRClientMT::thrdMain, this);
	m_thrdRx = std::thread(VRClientMT::thrdRxProcess, this);
	m_thrdTx = std::thread(VRClientMT::thrdTxProcess, this);

    ms_diaNumber = 0;
	m_RxState = 1;
	m_TxState = 1;
    m_totalVoiceDataLen =0 ;
    //thrd.detach();
	//printf("\t[DEBUG] VRClinetMT Constructed.\n");
    m_Logger = config->getLogger();
    m_Logger->debug("VRClinetMT Constructed.");
}


VRClientMT::~VRClientMT()
{
	QueItem* item;
	// while (!m_qRTQue.empty()) {
	// 	item = m_qRTQue.front();
	// 	m_qRTQue.pop();

	// 	delete[] item->voiceData;
	// 	delete item;
	// }

	while (!m_qRXQue.empty()) {
		item = m_qRXQue.front();
		m_qRXQue.pop();

		delete[] item->voiceData;
		delete item;
	}

	while (!m_qTXQue.empty()) {
		item = m_qTXQue.front();
		m_qTXQue.pop();

		delete[] item->voiceData;
		delete item;
	}

	//printf("\t[DEBUG] VRClinetMT Destructed.\n");
    m_Logger->debug("VRClinetMT Destructed.");
}

void VRClientMT::finish()
{
	m_nLiveFlag = 0;
}

#define BUFLEN (16000 * 10 + 64)  //Max length of buffer

typedef struct _posPair {
    uint64_t bpos;
    uint64_t epos;
} PosPair;

#define WAV_HEADER_SIZE 44
#define WAV_BUFF_SIZE 19200
#define MM_SIZE (WAV_HEADER_SIZE + WAV_BUFF_SIZE)

void VRClientMT::thrdMain(VRClientMT* client) {

    std::string sPubCannel = config->getConfig("redis.pubchannel", "RT-STT");
    xRedisClient &xRedis = client->getXRdedisClient();
    RedisDBIdx dbi(&xRedis);

    dbi.CreateDBIndex(client->getCallId().c_str(), APHash, CACHE_TYPE_1);

    auto t1 = std::chrono::high_resolution_clock::now();
      
    while (client->m_RxState || client->m_TxState)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));//microseconds(10));//milliseconds(1));
    }
    
    uint64_t totalVLen = client->getTotalVoiceDataLen();
    std::string svr_nm = client->getServerName();

    client->m_Mgr->removeVRC(client->m_sCallId);

#ifdef USE_XREDIS
    int64_t zCount=0;
    if (!xRedis.publish(dbi, sPubCannel.c_str(), client->getCallId().c_str(), zCount)) {
        client->m_Logger->error("VRClientMT::thrdMain(%s) - redis publish(). [%s], zCount(%d)", client->m_sCallId.c_str(), dbi.GetErrInfo(), zCount);
    }
#endif

    if (client->m_s2d) {
        auto t2 = std::chrono::high_resolution_clock::now();

        client->m_s2d->updateCallInfo(client->m_sCallId, true);
        client->m_s2d->updateTaskInfo(client->m_sCallId, std::string("MN"), client->m_sCounselCode, 'Y', totalVLen, totalVLen/16000, std::chrono::duration_cast<std::chrono::seconds>(t2-t1).count(), "TBL_JOB_INFO", "", svr_nm.c_str());
    }

    // HA
    if (HAManager::getInstance())
        HAManager::getInstance()->insertSyncItem(false, client->m_sCallId, client->m_sCounselCode, std::string("remove"), 1, 1);

    if (client->m_is_save_pcm) {
        if (config->isSet("stas.merge")) {
            std::string cmd = "";
            cmd = config->getConfig("stas.merge");
            cmd.push_back(' ');
            cmd.append(client->m_pcm_path.c_str());
            cmd.push_back(' ');
            cmd.append(client->m_sCallId.c_str());
            // job_log->debug("[%s, 0x%X] %s", job_name, THREAD_ID, cmd.c_str());
            if (std::system(cmd.c_str())) {
                client->m_Logger->error("VRClientMT::thrdMain(%s) Fail to merge wavs: command(%s)", client->m_sCallId.c_str(), cmd.c_str());
            }
        }
    }
    client->m_Logger->debug("VRClientMT::thrdMain(%s) - FINISH CALL.", client->m_sCallId.c_str());

	WorkTracer::instance()->insertWork(client->m_sCallId, client->m_cJobType, WorkQueItem::PROCTYPE::R_FREE_WORKER);

	client->m_thrd.detach();
	delete client;
}

void VRClientMT::thrdRxProcess(VRClientMT* client) {

	QueItem* item;
    gearman_client_st *gearClient;
    gearman_return_t ret;
    void *value = NULL;
    size_t result_size;
    gearman_return_t rc;
    PosPair stPos;
    WAVE_HEADER wHdr;
    
    char buf[BUFLEN];
    uint16_t nHeadLen=0;
    
    uint8_t *vpBuf = NULL;
    size_t posBuf = 0;
    std::vector<uint8_t> vBuff;
    size_t sframe;
    size_t eframe;
    uint64_t totalVoiceDataLen;
    size_t framelen;
    std::string svr_nm;

    vBuff.reserve(MM_SIZE);
    
    framelen = client->m_framelen * 2;

#ifdef USE_XREDIS
    iconv_t it;
    VALUES vVal;
    std::string sPubCannel = config->getConfig("redis.pubchannel", "RT-STT");
    xRedisClient &xRedis = client->getXRdedisClient();
    RedisDBIdx dbi(&xRedis);

    dbi.CreateDBIndex(client->getCallId().c_str(), APHash, CACHE_TYPE_1);
    it = iconv_open("UTF-8", "EUC-KR");
#endif

    memcpy(wHdr.Riff.ChunkID, "RIFF", 4);
    wHdr.Riff.ChunkSize = 0;
    memcpy(wHdr.Riff.Format, "WAVE", 4);

    memcpy(wHdr.Fmt.ChunkID, "fmt ", 4);
    wHdr.Fmt.ChunkSize = 16;
    wHdr.Fmt.AudioFormat = 1;
    wHdr.Fmt.NumChannels = 1;
    wHdr.Fmt.SampleRate = 8000;
    wHdr.Fmt.AvgByteRate = 8000 * 1 * 16 / 8 ;
    wHdr.Fmt.BlockAlign = 1 * 16 / 8;
    wHdr.Fmt.BitPerSample = 16;

    memcpy(wHdr.Data.ChunkID, "data", 4);
    wHdr.Data.ChunkSize = 0;

    if (client->m_is_save_pcm) {
        std::string filename = client->m_pcm_path + "/" + client->m_sCallId + std::string("_r.wav");
        std::ofstream pcmFile;

        pcmFile.open(filename, ios::out | ios::trunc | ios::binary);
        if (pcmFile.is_open()) {
            pcmFile.write((const char*)&wHdr, sizeof(WAVE_HEADER));
            pcmFile.close();
        }
    }
    
    stPos.bpos = 0;
    stPos.epos = 0;
    
    
    gearClient = gearman_client_create(NULL);
    if (!gearClient) {
        //printf("\t[DEBUG] VRClientMT::thrdRxProcess() - ERROR (Failed gearman_client_create - %s)\n", client->m_sCallId.c_str());
        client->m_Logger->error("VRClientMT::thrdRxProcess() - ERROR (Failed gearman_client_create - %s)", client->m_sCallId.c_str());

        // WorkTracer::instance()->insertWork(client->m_sCallId, client->m_cJobType, WorkQueItem::PROCTYPE::R_FREE_WORKER);

        client->m_thrdRx.detach();
        client->m_RxState = 0;
        // delete client;
        return;
    }
    
    ret= gearman_client_add_server(gearClient, client->m_sGearHost.c_str(), client->m_nGearPort);
    if (gearman_failed(ret))
    {
        //printf("\t[DEBUG] VRClientMT::thrdRxProcess() - ERROR (Failed gearman_client_add_server - %s)\n", client->m_sCallId.c_str());
        client->m_Logger->error("VRClientMT::thrdRxProcess() - ERROR (Failed gearman_client_add_server - %s)", client->m_sCallId.c_str());

        // WorkTracer::instance()->insertWork(client->m_sCallId, client->m_cJobType, WorkQueItem::PROCTYPE::R_FREE_WORKER);

        client->m_thrdRx.detach();
        client->m_RxState = 0;
        // delete client;
        return;
    }
    

	// m_cJobType에 따라 작업 형태를 달리해야 한다. 
	if (client->m_cJobType == 'R') {
        uint32_t diaNumber=1;   // DB 실시간 STT 테이블에 저장될 호(Call)단위 Index 값
        Fvad *vad = NULL;
        int vadres;
        int aDianum;

        vad = fvad_new();
        if (!vad) {//} || (fvad_set_sample_rate(vad, in_info.samplerate) < 0)) {
            client->m_Logger->error("VRClientMT::thrdRxProcess() - ERROR (Failed fvad_new(%s))", client->m_sCallId.c_str());
            gearman_client_free(gearClient);
            // WorkTracer::instance()->insertWork(client->m_sCallId, client->m_cJobType, WorkQueItem::PROCTYPE::R_FREE_WORKER);
            client->m_thrdRx.detach();
            client->m_RxState = 0;
            // delete client;
            return;
        }


		// 실시간의 경우 통화가 종료되기 전까지 Queue에서 입력 데이터를 받아 처리
		// FILE인 경우 기존과 동일하게 filename을 전달하는 방법 이용
        if (client->m_nGearTimeout) {
            gearman_client_set_timeout(gearClient, client->m_nGearTimeout);
        }
        
#if 0 // for DEBUG
		std::string filename = client->m_pcm_path + "/" + client->m_sCallId + std::string("_") + std::to_string(client->m_nNumofChannel) + std::string(".pcm");
		std::ofstream pcmFile;
        if (client->m_is_save_pcm)
            pcmFile.open(filename, ios::out | ios::app | ios::binary);
#endif

        // write wav heaer to file(mmap);
        vBuff.clear();
        sframe = 0;
        eframe = 0;
        aDianum = 0;
        totalVoiceDataLen = 0;
        svr_nm = "DEFAULT";

		while (client->m_nLiveFlag)
		{
			while (!client->m_qRXQue.empty()) {
				// g = new std::lock_guard<std::mutex>(client->m_mxQue);
				item = client->m_qRXQue.front();
				client->m_qRXQue.pop();
				// delete g;

                totalVoiceDataLen += item->lenVoiceData;

                stPos.epos += item->lenVoiceData;
				// queue에서 가져온 item을 STT 하는 로직을 아래에 코딩한다.
				// call-id + item->spkNo => call-id for rt-stt
                memset(buf, 0, sizeof(buf));
                if (!item->flag) {
                    sprintf(buf, "%s_%d|%s|", client->m_sCallId.c_str(), item->spkNo, "LAST");
                }
                else if (item->flag == 2) {
                    sprintf(buf, "%s_%d|%s|", client->m_sCallId.c_str(), item->spkNo, "FIRS");
                }
                else {
                    sprintf(buf, "%s_%d|%s|", client->m_sCallId.c_str(), item->spkNo, "NOLA");
                }
                nHeadLen = strlen(buf);

                if (vBuff.size()>0) {
                    for(int i=0; i<nHeadLen; i++) {
                        vBuff[i] = buf[i];
                    }
                }
                else {
                    for(int i=0; i<nHeadLen; i++) {
                        vBuff.push_back(buf[i]);
                    }
                }
                

                if (client->m_is_save_pcm) {
                    std::string filename = client->m_pcm_path + "/" + client->m_sCallId + std::string("_r.wav");
                    std::ofstream pcmFile;

                    pcmFile.open(filename, ios::out | ios::app | ios::binary);
                    if (pcmFile.is_open()) {
                        pcmFile.write((const char*)item->voiceData, item->lenVoiceData);
                        pcmFile.close();
                    }
                }
                
                // check vad!, by loop()
                // if finish check vad and vBuff is no empty, send buff to VR by gearman
                // vadres == 1 vBuff[item->spkNo-1].push_back();
                // vadres == 0 and vBuff[item->spkNo-1].size() > 0 then send buff to gearman
                posBuf = 0;
                while ((item->lenVoiceData >= framelen) && ((item->lenVoiceData - posBuf) >= framelen)) {
                    vpBuf = (uint8_t *)(item->voiceData+posBuf);
                    eframe += (client->m_framelen/8);
                    // Convert the read samples to int16
                    vadres = fvad_process(vad, (const int16_t *)vpBuf, client->m_framelen);

                    //client->m_Logger->debug("VRClientMT::thrdMain(%s) - SUB WHILE... [%d : %d], timeout(%d)", client->m_sCallId.c_str(), sframe[item->spkNo -1], eframe[item->spkNo -1], client->m_nGearTimeout);

                    if (vadres < 0) {
                        //client->m_Logger->error("VRClientMT::thrdMain(%d, %d, %s)(%s) - send buffer buff_len(%lu), spos(%lu), epos(%lu)", nHeadLen, item->spkNo, buf, client->m_sCallId.c_str(), vBuff[item->spkNo-1].size(), sframe[item->spkNo-1], eframe[item->spkNo-1]);
                        continue;
                    }

                    if (vadres > 0) {
                        for(size_t i=0; i<framelen; i++) {
                            vBuff.push_back(vpBuf[i]);
                            
                        }
                    }
                    
                    if (!vadres && (vBuff.size()<=nHeadLen)) {
                        // start ms
                        sframe = eframe - 20;
                    }

                    if (!vadres && (vBuff.size()>nHeadLen)) {
                        if (vBuff.size() > 15000) {   // 8000 bytes, 0.5 이하의 음성데이터는 처리하지 않음
                            // send buff to gearman
                            if (aDianum == 0) {
                                sprintf(buf, "%s_%d|%s|", client->m_sCallId.c_str(), item->spkNo, "FIRS");
                            }
                            else {
                                sprintf(buf, "%s_%d|%s|", client->m_sCallId.c_str(), item->spkNo, "NOLA");
                            }
                            for(size_t i=0; i<strlen(buf); i++) {
                                vBuff[i] = buf[i];
                            }
                            //client->m_Logger->debug("VRClientMT::thrdMain(%d, %d, %s)(%s) - send buffer buff_len(%lu), spos(%lu), epos(%lu)", nHeadLen, item->spkNo, buf, client->m_sCallId.c_str(), vBuff[item->spkNo-1].size(), sframe[item->spkNo-1], eframe[item->spkNo-1]);
                            value= gearman_client_do(gearClient, "vr_realtime", NULL, 
                                                            (const void*)&vBuff[0], vBuff.size(),
                                                            &result_size, &rc);
                                                            
                            aDianum++;
                            
                            if (gearman_success(rc))
                            {
                                // Make use of value
                                if (value) {
                                    std::string modValue = boost::replace_all_copy(std::string((const char*)value), "\n", " ");
                                    // std::cout << "DEBUG : value(" << (char *)value << ") : size(" << result_size << ")" << std::endl;
                                    //client->m_Logger->debug("VRClientMT::thrdMain(%s) - sttIdx(%d)\nsrc(%s)\ndst(%s)", client->m_sCallId.c_str(), sttIdx, srcBuff, dstBuff);
                                    diaNumber = client->getDiaNumber();
#ifdef USE_XREDIS
                                    int64_t zCount=0;
                                    std::string sJsonValue;

                                    size_t in_size, out_size;
                                    // iconv_t it;
                                    char *utf_buf = NULL;
                                    char *input_buf_ptr = NULL;
                                    char *output_buf_ptr = NULL;

                                    in_size = modValue.size();
                                    out_size = in_size * 2 + 1;
                                    utf_buf = (char *)malloc(out_size);

                                    if (utf_buf) {
                                        memset(utf_buf, 0, out_size);

                                        input_buf_ptr = (char *)modValue.c_str();
                                        output_buf_ptr = utf_buf;

                                        // it = iconv_open("UTF-8", "EUC-KR");

                                        iconv(it, &input_buf_ptr, &in_size, &output_buf_ptr, &out_size);
                                        
                                        // iconv_close(it);
                                        

                                        {
                                            rapidjson::Document d;
                                            rapidjson::Document::AllocatorType& alloc = d.GetAllocator();

                                            d.SetObject();
                                            d.AddMember("IDX", diaNumber, alloc);
                                            d.AddMember("CALL_ID", rapidjson::Value(client->getCallId().c_str(), alloc).Move(), alloc);
                                            d.AddMember("SPK", rapidjson::Value("R", alloc).Move(), alloc);
                                            d.AddMember("POS_START", sframe/10, alloc);
                                            d.AddMember("POS_END", eframe/10, alloc);
                                            d.AddMember("VALUE", rapidjson::Value(utf_buf, alloc).Move(), alloc);

                                            rapidjson::StringBuffer strbuf;
                                            rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);
                                            d.Accept(writer);

                                            sJsonValue = strbuf.GetString();
                                        }

                                        vVal.push_back(toString(diaNumber));
                                        vVal.push_back(sJsonValue);

                                        if ( !xRedis.zadd(dbi, client->getCallId(), vVal, zCount) ) {
                                            client->m_Logger->error("VRClientMT::thrdMain(%s) - redis zadd(). [%s], zCount(%d)", client->m_sCallId.c_str(), dbi.GetErrInfo(), zCount);
                                        }

                                        free(utf_buf);
                                    }
#endif
                                    // to DB
                                    if (client->m_s2d) {
                                        client->m_s2d->insertSTTData(diaNumber, client->m_sCallId, item->spkNo, sframe/10, eframe/10, modValue);
                                    }
                                    //STTDeliver::instance(client->m_Logger)->insertSTT(client->m_sCallId, std::string((const char*)value), item->spkNo, vPos[item->spkNo -1].bpos, vPos[item->spkNo -1].epos);
                                    // to STTDeliver(file)
                                    if (client->m_deliver) {
                                        client->m_deliver->insertSTT(client->m_sCallId, modValue, item->spkNo, sframe/10, eframe/10);
                                    }

                                    free(value);
                                    
                                }
                            }
                            else if (gearman_failed(rc)){
                                client->m_Logger->error("VRClientMT::thrdRxProcess(%s) - failed gearman_client_do(). [%lu : %lu], timeout(%d)", client->m_sCallId.c_str(), sframe, eframe, client->m_nGearTimeout);
                            }
                        }

                        // and clear buff, set msg header
                        vBuff.clear();

                        for(size_t i=0; i<nHeadLen; i++) {
                            //vBuff[item->spkNo-1][i] = buf[i];
                            vBuff.push_back(buf[i]);

                        }
                        sframe = eframe;
                    }
                    
                    posBuf += framelen;
                }

				if (!item->flag) {	// 호가 종료되었음을 알리는 flag, 채널 갯수와 flag(0)이 들어온 갯수를 비교해야한다.
					//printf("\t[DEBUG] VRClientMT::thrdRxProcess(%s) - final item delivered.\n", client->m_sCallId.c_str());
                    client->m_Logger->debug("VRClientMT::thrdRxProcess(%s, %d) - final item delivered.", client->m_sCallId.c_str(), item->spkNo);

                    // send buff to gearman
                    sprintf(buf, "%s_%d|%s|", client->m_sCallId.c_str(), item->spkNo, "LAST");
                    if (vBuff.size() > 0) {
                        for(size_t i=0; i<strlen(buf); i++) {
                            vBuff[i] = buf[i];
                        }
                    }
                    else {
                        for(size_t i=0; i<strlen(buf); i++) {
                            vBuff.push_back(buf[i]);
                        }
                    }
                    value= gearman_client_do(gearClient, "vr_realtime", NULL, 
                                                    (const void*)&vBuff[0], vBuff.size(),
                                                    &result_size, &rc);
                    if (gearman_success(rc))
                    {
                        std::string svalue = (const char*)value;
                        svr_nm = svalue.substr(0, svalue.find("\n"));

                        free(value);

                        svalue.erase(0, svalue.find("\n")+1);
                        // Make use of value
                        if (svr_nm.size() && svalue.size()) {
                            std::string modValue = boost::replace_all_copy(svalue, "\n", " ");
                            // std::cout << "DEBUG : value(" << (char *)value << ") : size(" << result_size << ")" << std::endl;
                            //client->m_Logger->debug("VRClientMT::thrdMain(%s) - sttIdx(%d)\nsrc(%s)\ndst(%s)", client->m_sCallId.c_str(), sttIdx, srcBuff, dstBuff);
                            diaNumber = client->getDiaNumber();
#ifdef USE_XREDIS
                            int64_t zCount=0;
                            std::string sJsonValue;
                            size_t in_size, out_size;
                            // iconv_t it;
                            char *utf_buf = NULL;
                            char *input_buf_ptr = NULL;
                            char *output_buf_ptr = NULL;

                            in_size = modValue.size();
                            out_size = in_size * 2 + 1;
                            utf_buf = (char *)malloc(out_size);

                            if (utf_buf) {
                                memset(utf_buf, 0, out_size);

                                input_buf_ptr = (char *)modValue.c_str();
                                output_buf_ptr = utf_buf;

                                // it = iconv_open("UTF-8", "EUC-KR");

                                iconv(it, &input_buf_ptr, &in_size, &output_buf_ptr, &out_size);
                                
                                // iconv_close(it);

                                {
                                    rapidjson::Document d;
                                    rapidjson::Document::AllocatorType& alloc = d.GetAllocator();

                                    d.SetObject();
                                    d.AddMember("IDX", diaNumber, alloc);
                                    d.AddMember("CALL_ID", rapidjson::Value(client->getCallId().c_str(), alloc).Move(), alloc);
                                    d.AddMember("SPK", rapidjson::Value("R", alloc).Move(), alloc);
                                    d.AddMember("POS_START", sframe/10, alloc);
                                    d.AddMember("POS_END", eframe/10, alloc);
                                    d.AddMember("VALUE", rapidjson::Value(utf_buf, alloc).Move(), alloc);

                                    rapidjson::StringBuffer strbuf;
                                    rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);
                                    d.Accept(writer);

                                    sJsonValue = strbuf.GetString();
                                }

                                vVal.push_back(toString(diaNumber));
                                vVal.push_back(sJsonValue);

                                
                                // vVal.push_back(toString(diaNumber));
                                // vVal.push_back(modValue);

                                if ( !xRedis.zadd(dbi, client->getCallId(), vVal, zCount) ) {
                                    client->m_Logger->error("VRClientMT::thrdRxProcess(%s) - redis zadd(). [%s], zCount(%d)", client->m_sCallId.c_str(), dbi.GetErrInfo(), zCount);
                                }

                                free(utf_buf);
                            }
#endif

                            if (client->m_s2d) {
                                client->m_s2d->insertSTTData(diaNumber, client->m_sCallId, item->spkNo, sframe/10, eframe/10, modValue);
                            }
                            //STTDeliver::instance(client->m_Logger)->insertSTT(client->m_sCallId, std::string((const char*)value), item->spkNo, vPos[item->spkNo -1].bpos, vPos[item->spkNo -1].epos);
                            // to STTDeliver(file)
                            if (client->m_deliver) {
                                client->m_deliver->insertSTT(client->m_sCallId, modValue, item->spkNo, sframe/10, eframe/10);
                            }
                            
                        }
                    }
                    else if (gearman_failed(rc)){
                        client->m_Logger->error("VRClientMT::thrdRxProcess(%s) - failed gearman_client_do(). [%d : %d], timeout(%d)", client->m_sCallId.c_str(), sframe, eframe, client->m_nGearTimeout);
                    }

                    // and clear buff, set msg header
                    vBuff.clear();

                    if ( item->voiceData != NULL ) delete[] item->voiceData;
                    delete item;

                    if (client->m_is_save_pcm) {
                        std::string filename = client->m_pcm_path + "/" + client->m_sCallId + std::string("_r.wav");
                        std::ofstream pcmFile;

                        wHdr.Riff.ChunkSize = totalVoiceDataLen + sizeof(WAVE_HEADER) - 8;
                        wHdr.Data.ChunkSize = totalVoiceDataLen;

                        pcmFile.open(filename, ios::in | ios::out | ios::binary);
                        if (pcmFile.is_open()) {
                            pcmFile.seekp(0);
                            pcmFile.write((const char*)&wHdr, sizeof(WAVE_HEADER));
                            pcmFile.close();
                        }
                    }

                    client->m_Logger->debug("VRClientMT::thrdRxProcess(%s) - FINISH CALL.", client->m_sCallId.c_str());
                    break;
				}

				delete[] item->voiceData;
				delete item;
				// 예외 발생 시 처리 내용 : VDCManager의 removeVDC를 호출할 수 있어야 한다. - 이 후 VRClient는 item->flag(0)에 대해서만 처리한다.
			}
            //client->m_Logger->debug("VRClientMT::thrdMain(%s) - WHILE... [%d : %d], timeout(%d)", client->m_sCallId.c_str(), sframe[item->spkNo -1], eframe[item->spkNo -1], client->m_nGearTimeout);
			std::this_thread::sleep_for(std::chrono::microseconds(10));//milliseconds(1));
		}
        
        client->setServerName(svr_nm);
        client->setTotalVoiceDataLen(totalVoiceDataLen);

        fvad_free(vad);

        std::vector<uint8_t>().swap(vBuff);

#if 0 // for DEBUG
		if (client->m_is_save_pcm && pcmFile.is_open()) pcmFile.close();
#endif
	}

    gearman_client_free(gearClient);

#ifdef USE_XREDIS
    iconv_close(it);
#endif

	client->m_thrdRx.detach();
    client->m_RxState = 0;
	// delete client;
}

void VRClientMT::thrdTxProcess(VRClientMT* client) {

	QueItem* item;
    gearman_client_st *gearClient;
    gearman_return_t ret;
    void *value = NULL;
    size_t result_size;
    gearman_return_t rc;
    PosPair stPos;
    WAVE_HEADER wHdr;
    
    char buf[BUFLEN];
    uint16_t nHeadLen=0;
    
    uint8_t *vpBuf = NULL;
    size_t posBuf = 0;
    std::vector<uint8_t> vBuff;
    size_t sframe;
    size_t eframe;
    uint64_t totalVoiceDataLen;
    size_t framelen;
    std::string svr_nm;

    vBuff.reserve(MM_SIZE);
    
    framelen = client->m_framelen * 2;

#ifdef USE_XREDIS
    iconv_t it;
    VALUES vVal;
    std::string sPubCannel = config->getConfig("redis.pubchannel", "RT-STT");
    xRedisClient &xRedis = client->getXRdedisClient();
    RedisDBIdx dbi(&xRedis);

    dbi.CreateDBIndex(client->getCallId().c_str(), APHash, CACHE_TYPE_1);
    it = iconv_open("UTF-8", "EUC-KR");
#endif

    memcpy(wHdr.Riff.ChunkID, "RIFF", 4);
    wHdr.Riff.ChunkSize = 0;
    memcpy(wHdr.Riff.Format, "WAVE", 4);

    memcpy(wHdr.Fmt.ChunkID, "fmt ", 4);
    wHdr.Fmt.ChunkSize = 16;
    wHdr.Fmt.AudioFormat = 1;
    wHdr.Fmt.NumChannels = 1;
    wHdr.Fmt.SampleRate = 8000;
    wHdr.Fmt.AvgByteRate = 8000 * 1 * 16 / 8 ;
    wHdr.Fmt.BlockAlign = 1 * 16 / 8;
    wHdr.Fmt.BitPerSample = 16;

    memcpy(wHdr.Data.ChunkID, "data", 4);
    wHdr.Data.ChunkSize = 0;

    if (client->m_is_save_pcm) {
        std::string filename = client->m_pcm_path + "/" + client->m_sCallId + std::string("_l.wav");
        std::ofstream pcmFile;

        pcmFile.open(filename, ios::out | ios::trunc | ios::binary);
        if (pcmFile.is_open()) {
            pcmFile.write((const char*)&wHdr, sizeof(WAVE_HEADER));
            pcmFile.close();
        }
    }
    
    stPos.bpos = 0;
    stPos.epos = 0;
    
    
    gearClient = gearman_client_create(NULL);
    if (!gearClient) {
        //printf("\t[DEBUG] VRClientMT::thrdTxProcess() - ERROR (Failed gearman_client_create - %s)\n", client->m_sCallId.c_str());
        client->m_Logger->error("VRClientMT::thrdTxProcess() - ERROR (Failed gearman_client_create - %s)", client->m_sCallId.c_str());

        // WorkTracer::instance()->insertWork(client->m_sCallId, client->m_cJobType, WorkQueItem::PROCTYPE::R_FREE_WORKER);

        client->m_thrdTx.detach();
        client->m_TxState = 0;
        // delete client;
        return;
    }
    
    ret= gearman_client_add_server(gearClient, client->m_sGearHost.c_str(), client->m_nGearPort);
    if (gearman_failed(ret))
    {
        //printf("\t[DEBUG] VRClientMT::thrdTxProcess() - ERROR (Failed gearman_client_add_server - %s)\n", client->m_sCallId.c_str());
        client->m_Logger->error("VRClientMT::thrdTxProcess() - ERROR (Failed gearman_client_add_server - %s)", client->m_sCallId.c_str());

        // WorkTracer::instance()->insertWork(client->m_sCallId, client->m_cJobType, WorkQueItem::PROCTYPE::R_FREE_WORKER);

        client->m_thrdTx.detach();
        client->m_TxState = 0;
        // delete client;
        return;
    }
    

	// m_cJobType에 따라 작업 형태를 달리해야 한다. 
	if (client->m_cJobType == 'R') {
        uint32_t diaNumber=1;   // DB 실시간 STT 테이블에 저장될 호(Call)단위 Index 값
        Fvad *vad = NULL;
        int vadres;
        int aDianum;

        vad = fvad_new();
        if (!vad) {//} || (fvad_set_sample_rate(vad, in_info.samplerate) < 0)) {
            client->m_Logger->error("VRClientMT::thrdTxProcess() - ERROR (Failed fvad_new(%s))", client->m_sCallId.c_str());
            gearman_client_free(gearClient);
            // WorkTracer::instance()->insertWork(client->m_sCallId, client->m_cJobType, WorkQueItem::PROCTYPE::R_FREE_WORKER);
            client->m_thrdTx.detach();
            client->m_TxState = 0;
            // delete client;
            return;
        }


		// 실시간의 경우 통화가 종료되기 전까지 Queue에서 입력 데이터를 받아 처리
		// FILE인 경우 기존과 동일하게 filename을 전달하는 방법 이용
        if (client->m_nGearTimeout) {
            gearman_client_set_timeout(gearClient, client->m_nGearTimeout);
        }
        
#if 0 // for DEBUG
		std::string filename = client->m_pcm_path + "/" + client->m_sCallId + std::string("_") + std::to_string(client->m_nNumofChannel) + std::string(".pcm");
		std::ofstream pcmFile;
        if (client->m_is_save_pcm)
            pcmFile.open(filename, ios::out | ios::app | ios::binary);
#endif

        // write wav heaer to file(mmap);
        vBuff.clear();
        sframe = 0;
        eframe = 0;
        aDianum = 0;
        totalVoiceDataLen = 0;
        svr_nm = "DEFAULT";

		while (client->m_nLiveFlag)
		{
			while (!client->m_qTXQue.empty()) {
				// g = new std::lock_guard<std::mutex>(client->m_mxQue);
				item = client->m_qTXQue.front();
				client->m_qTXQue.pop();
				// delete g;

                totalVoiceDataLen += item->lenVoiceData;

                stPos.epos += item->lenVoiceData;
				// queue에서 가져온 item을 STT 하는 로직을 아래에 코딩한다.
				// call-id + item->spkNo => call-id for rt-stt
                memset(buf, 0, sizeof(buf));
                if (!item->flag) {
                    sprintf(buf, "%s_%d|%s|", client->m_sCallId.c_str(), item->spkNo, "LAST");
                }
                else if (item->flag == 2) {
                    sprintf(buf, "%s_%d|%s|", client->m_sCallId.c_str(), item->spkNo, "FIRS");
                }
                else {
                    sprintf(buf, "%s_%d|%s|", client->m_sCallId.c_str(), item->spkNo, "NOLA");
                }
                nHeadLen = strlen(buf);

                if (vBuff.size()>0) {
                    for(int i=0; i<nHeadLen; i++) {
                        vBuff[i] = buf[i];
                    }
                }
                else {
                    for(int i=0; i<nHeadLen; i++) {
                        vBuff.push_back(buf[i]);
                    }
                }
                

                if (client->m_is_save_pcm) {
                    std::string filename = client->m_pcm_path + "/" + client->m_sCallId + std::string("_l.wav");
                    std::ofstream pcmFile;

                    pcmFile.open(filename, ios::out | ios::app | ios::binary);
                    if (pcmFile.is_open()) {
                        pcmFile.write((const char*)item->voiceData, item->lenVoiceData);
                        pcmFile.close();
                    }
                }
                
                // check vad!, by loop()
                // if finish check vad and vBuff is no empty, send buff to VR by gearman
                // vadres == 1 vBuff[item->spkNo-1].push_back();
                // vadres == 0 and vBuff[item->spkNo-1].size() > 0 then send buff to gearman
                posBuf = 0;
                while ((item->lenVoiceData >= framelen) && ((item->lenVoiceData - posBuf) >= framelen)) {
                    vpBuf = (uint8_t *)(item->voiceData+posBuf);
                    eframe += (client->m_framelen/8);
                    // Convert the read samples to int16
                    vadres = fvad_process(vad, (const int16_t *)vpBuf, client->m_framelen);

                    //client->m_Logger->debug("VRClientMT::thrdMain(%s) - SUB WHILE... [%d : %d], timeout(%d)", client->m_sCallId.c_str(), sframe[item->spkNo -1], eframe[item->spkNo -1], client->m_nGearTimeout);

                    if (vadres < 0) {
                        //client->m_Logger->error("VRClientMT::thrdMain(%d, %d, %s)(%s) - send buffer buff_len(%lu), spos(%lu), epos(%lu)", nHeadLen, item->spkNo, buf, client->m_sCallId.c_str(), vBuff[item->spkNo-1].size(), sframe[item->spkNo-1], eframe[item->spkNo-1]);
                        continue;
                    }

                    if (vadres > 0) {
                        for(size_t i=0; i<framelen; i++) {
                            vBuff.push_back(vpBuf[i]);
                            
                        }
                    }
                    
                    if (!vadres && (vBuff.size()<=nHeadLen)) {
                        // start ms
                        sframe = eframe - 20;
                    }

                    if (!vadres && (vBuff.size()>nHeadLen)) {
                        if (vBuff.size() > 15000) {   // 8000 bytes, 0.5 이하의 음성데이터는 처리하지 않음
                            // send buff to gearman
                            if (aDianum == 0) {
                                sprintf(buf, "%s_%d|%s|", client->m_sCallId.c_str(), item->spkNo, "FIRS");
                            }
                            else {
                                sprintf(buf, "%s_%d|%s|", client->m_sCallId.c_str(), item->spkNo, "NOLA");
                            }
                            for(size_t i=0; i<strlen(buf); i++) {
                                vBuff[i] = buf[i];
                            }
                            //client->m_Logger->debug("VRClientMT::thrdMain(%d, %d, %s)(%s) - send buffer buff_len(%lu), spos(%lu), epos(%lu)", nHeadLen, item->spkNo, buf, client->m_sCallId.c_str(), vBuff[item->spkNo-1].size(), sframe[item->spkNo-1], eframe[item->spkNo-1]);
                            value= gearman_client_do(gearClient, "vr_realtime", NULL, 
                                                            (const void*)&vBuff[0], vBuff.size(),
                                                            &result_size, &rc);
                                                            
                            aDianum++;
                            
                            if (gearman_success(rc))
                            {
                                // Make use of value
                                if (value) {
                                    std::string modValue = boost::replace_all_copy(std::string((const char*)value), "\n", " ");
                                    // std::cout << "DEBUG : value(" << (char *)value << ") : size(" << result_size << ")" << std::endl;
                                    //client->m_Logger->debug("VRClientMT::thrdMain(%s) - sttIdx(%d)\nsrc(%s)\ndst(%s)", client->m_sCallId.c_str(), sttIdx, srcBuff, dstBuff);
                                    diaNumber = client->getDiaNumber();
#ifdef USE_XREDIS
                                    int64_t zCount=0;
                                    std::string sJsonValue;

                                    size_t in_size, out_size;
                                    // iconv_t it;
                                    char *utf_buf = NULL;
                                    char *input_buf_ptr = NULL;
                                    char *output_buf_ptr = NULL;

                                    in_size = modValue.size();
                                    out_size = in_size * 2 + 1;
                                    utf_buf = (char *)malloc(out_size);

                                    if (utf_buf) {
                                        memset(utf_buf, 0, out_size);

                                        input_buf_ptr = (char *)modValue.c_str();
                                        output_buf_ptr = utf_buf;

                                        iconv(it, &input_buf_ptr, &in_size, &output_buf_ptr, &out_size);

                                        {
                                            rapidjson::Document d;
                                            rapidjson::Document::AllocatorType& alloc = d.GetAllocator();

                                            d.SetObject();
                                            d.AddMember("IDX", diaNumber, alloc);
                                            d.AddMember("CALL_ID", rapidjson::Value(client->getCallId().c_str(), alloc).Move(), alloc);
                                            d.AddMember("SPK", rapidjson::Value("R", alloc).Move(), alloc);
                                            d.AddMember("POS_START", sframe/10, alloc);
                                            d.AddMember("POS_END", eframe/10, alloc);
                                            d.AddMember("VALUE", rapidjson::Value(utf_buf, alloc).Move(), alloc);

                                            rapidjson::StringBuffer strbuf;
                                            rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);
                                            d.Accept(writer);

                                            sJsonValue = strbuf.GetString();
                                        }

                                        vVal.push_back(toString(diaNumber));
                                        vVal.push_back(sJsonValue);

                                        if ( !xRedis.zadd(dbi, client->getCallId(), vVal, zCount) ) {
                                            client->m_Logger->error("VRClientMT::thrdMain(%s) - redis zadd(). [%s], zCount(%d)", client->m_sCallId.c_str(), dbi.GetErrInfo(), zCount);
                                        }

                                        free(utf_buf);
                                    }
#endif
                                    // to DB
                                    if (client->m_s2d) {
                                        client->m_s2d->insertSTTData(diaNumber, client->m_sCallId, item->spkNo, sframe/10, eframe/10, modValue);
                                    }
                                    //STTDeliver::instance(client->m_Logger)->insertSTT(client->m_sCallId, std::string((const char*)value), item->spkNo, vPos[item->spkNo -1].bpos, vPos[item->spkNo -1].epos);
                                    // to STTDeliver(file)
                                    if (client->m_deliver) {
                                        client->m_deliver->insertSTT(client->m_sCallId, modValue, item->spkNo, sframe/10, eframe/10);
                                    }

                                    free(value);
                                    
                                }
                            }
                            else if (gearman_failed(rc)){
                                client->m_Logger->error("VRClientMT::thrdTxProcess(%s) - failed gearman_client_do(). [%lu : %lu], timeout(%d)", client->m_sCallId.c_str(), sframe, eframe, client->m_nGearTimeout);
                            }
                        }

                        // and clear buff, set msg header
                        vBuff.clear();

                        for(size_t i=0; i<nHeadLen; i++) {
                            //vBuff[item->spkNo-1][i] = buf[i];
                            vBuff.push_back(buf[i]);

                        }
                        sframe = eframe;
                    }
                    
                    posBuf += framelen;
                }

				if (!item->flag) {	// 호가 종료되었음을 알리는 flag, 채널 갯수와 flag(0)이 들어온 갯수를 비교해야한다.
					//printf("\t[DEBUG] VRClientMT::thrdTxProcess(%s) - final item delivered.\n", client->m_sCallId.c_str());
                    client->m_Logger->debug("VRClientMT::thrdTxProcess(%s, %d) - final item delivered.", client->m_sCallId.c_str(), item->spkNo);

                    // send buff to gearman
                    sprintf(buf, "%s_%d|%s|", client->m_sCallId.c_str(), item->spkNo, "LAST");
                    if (vBuff.size() > 0) {
                        for(size_t i=0; i<strlen(buf); i++) {
                            vBuff[i] = buf[i];
                        }
                    }
                    else {
                        for(size_t i=0; i<strlen(buf); i++) {
                            vBuff.push_back(buf[i]);
                        }
                    }
                    value= gearman_client_do(gearClient, "vr_realtime", NULL, 
                                                    (const void*)&vBuff[0], vBuff.size(),
                                                    &result_size, &rc);
                    if (gearman_success(rc))
                    {
                        std::string svalue = (const char*)value;
                        svr_nm = svalue.substr(0, svalue.find("\n"));

                        free(value);

                        svalue.erase(0, svalue.find("\n")+1);
                        // Make use of value
                        if (svr_nm.size() && svalue.size()) {
                            std::string modValue = boost::replace_all_copy(svalue, "\n", " ");
                            // std::cout << "DEBUG : value(" << (char *)value << ") : size(" << result_size << ")" << std::endl;
                            //client->m_Logger->debug("VRClientMT::thrdMain(%s) - sttIdx(%d)\nsrc(%s)\ndst(%s)", client->m_sCallId.c_str(), sttIdx, srcBuff, dstBuff);
                            diaNumber = client->getDiaNumber();
#ifdef USE_XREDIS
                            int64_t zCount=0;
                            std::string sJsonValue;
                            size_t in_size, out_size;
                            // iconv_t it;
                            char *utf_buf = NULL;
                            char *input_buf_ptr = NULL;
                            char *output_buf_ptr = NULL;

                            in_size = modValue.size();
                            out_size = in_size * 2 + 1;
                            utf_buf = (char *)malloc(out_size);

                            if (utf_buf) {
                                memset(utf_buf, 0, out_size);

                                input_buf_ptr = (char *)modValue.c_str();
                                output_buf_ptr = utf_buf;

                                iconv(it, &input_buf_ptr, &in_size, &output_buf_ptr, &out_size);

                                {
                                    rapidjson::Document d;
                                    rapidjson::Document::AllocatorType& alloc = d.GetAllocator();

                                    d.SetObject();
                                    d.AddMember("IDX", diaNumber, alloc);
                                    d.AddMember("CALL_ID", rapidjson::Value(client->getCallId().c_str(), alloc).Move(), alloc);
                                    d.AddMember("SPK", rapidjson::Value("R", alloc).Move(), alloc);
                                    d.AddMember("POS_START", sframe/10, alloc);
                                    d.AddMember("POS_END", eframe/10, alloc);
                                    d.AddMember("VALUE", rapidjson::Value(utf_buf, alloc).Move(), alloc);

                                    rapidjson::StringBuffer strbuf;
                                    rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);
                                    d.Accept(writer);

                                    sJsonValue = strbuf.GetString();
                                }

                                vVal.push_back(toString(diaNumber));
                                vVal.push_back(sJsonValue);

                                if ( !xRedis.zadd(dbi, client->getCallId(), vVal, zCount) ) {
                                    client->m_Logger->error("VRClientMT::thrdTxProcess(%s) - redis zadd(). [%s], zCount(%d)", client->m_sCallId.c_str(), dbi.GetErrInfo(), zCount);
                                }

                                free(utf_buf);
                            }
#endif

                            if (client->m_s2d) {
                                client->m_s2d->insertSTTData(diaNumber, client->m_sCallId, item->spkNo, sframe/10, eframe/10, modValue);
                            }
                            //STTDeliver::instance(client->m_Logger)->insertSTT(client->m_sCallId, std::string((const char*)value), item->spkNo, vPos[item->spkNo -1].bpos, vPos[item->spkNo -1].epos);
                            // to STTDeliver(file)
                            if (client->m_deliver) {
                                client->m_deliver->insertSTT(client->m_sCallId, modValue, item->spkNo, sframe/10, eframe/10);
                            }
                            
                        }
                    }
                    else if (gearman_failed(rc)){
                        client->m_Logger->error("VRClientMT::thrdTxProcess(%s) - failed gearman_client_do(). [%d : %d], timeout(%d)", client->m_sCallId.c_str(), sframe, eframe, client->m_nGearTimeout);
                    }

                    // and clear buff, set msg header
                    vBuff.clear();

                    if ( item->voiceData != NULL ) delete[] item->voiceData;
                    delete item;

                    if (client->m_is_save_pcm) {
                        std::string filename = client->m_pcm_path + "/" + client->m_sCallId + std::string("_l.wav");
                        std::ofstream pcmFile;

                        wHdr.Riff.ChunkSize = totalVoiceDataLen + sizeof(WAVE_HEADER) - 8;
                        wHdr.Data.ChunkSize = totalVoiceDataLen;

                        pcmFile.open(filename, ios::in | ios::out | ios::binary);
                        if (pcmFile.is_open()) {
                            pcmFile.seekp(0);
                            pcmFile.write((const char*)&wHdr, sizeof(WAVE_HEADER));
                            pcmFile.close();
                        }
                    }

                    client->m_Logger->debug("VRClientMT::thrdTxProcess(%s) - FINISH CALL.", client->m_sCallId.c_str());
                    break;
				}

				delete[] item->voiceData;
				delete item;
				// 예외 발생 시 처리 내용 : VDCManager의 removeVDC를 호출할 수 있어야 한다. - 이 후 VRClient는 item->flag(0)에 대해서만 처리한다.
			}
            //client->m_Logger->debug("VRClientMT::thrdMain(%s) - WHILE... [%d : %d], timeout(%d)", client->m_sCallId.c_str(), sframe[item->spkNo -1], eframe[item->spkNo -1], client->m_nGearTimeout);
			std::this_thread::sleep_for(std::chrono::microseconds(10));//milliseconds(1));
		}
        
        client->setServerName(svr_nm);
        client->setTotalVoiceDataLen(totalVoiceDataLen);

        fvad_free(vad);

        std::vector<uint8_t>().swap(vBuff);

#if 0 // for DEBUG
		if (client->m_is_save_pcm && pcmFile.is_open()) pcmFile.close();
#endif
	}

    gearman_client_free(gearClient);

#ifdef USE_XREDIS
    iconv_close(it);
#endif

	client->m_thrdTx.detach();
    client->m_TxState = 0;
}

void VRClientMT::insertQueItem(QueItem* item)
{
	std::lock_guard<std::mutex> g(m_mxQue);
	// m_qRTQue.push(item);

    if (item->spkNo == 1)
        m_qRXQue.push(item);
    else if (item->spkNo == 2)
        m_qTXQue.push(item);
    else {
        delete[] item->voiceData;
        delete item;
    }
}

#ifdef USE_XREDIS
xRedisClient& VRClientMT::getXRdedisClient()
{
    return m_Mgr->getRedisClient();
}
#endif


uint32_t VRClientMT::getDiaNumber()
{
    return ++ms_diaNumber;
}