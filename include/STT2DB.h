#pragma once

#include <stdint.h>

#include <string>
#include <queue>
#include <thread>
#include <mutex>
#include <vector>

#include <log4cpp/Category.hh>

#include <zdb.h>


class JobInfoItem {
    std::string m_callid;
    std::string m_counselorcode;
    std::string m_path;
    std::string m_filename;

    public:
    JobInfoItem(std::string callid, std::string counselorcode, std::string path, std::string filename);
    virtual ~JobInfoItem();

    std::string getCallId() { return m_callid; }
    std::string getCounselorCode() { return m_counselorcode; }
    std::string getPath() { return m_path; }
    std::string getFilename() {return m_filename;}
};

class RTSTTQueItem {
    uint32_t m_nDiaIdx;
	std::string m_sCallId;
	uint8_t m_nSpkNo;		// 1 ~ 9 : 통화자 구분값, 0 : 작업타입이 File인 경우
	std::string m_sSTTValue;

	uint64_t m_nBpos;
	uint64_t m_nEpos;

public:
	RTSTTQueItem(uint32_t idx, std::string callid, uint8_t spkno, std::string sttvalue, uint64_t bpos, uint64_t epos);
	virtual ~RTSTTQueItem();

	uint32_t getDiaIdx() { return m_nDiaIdx; }
	std::string& getCallId() { return m_sCallId; }
	uint8_t getSpkNo() { return m_nSpkNo; }
	std::string& getSTTValue() { return m_sSTTValue; }
    uint64_t getBpos() { return m_nBpos; }
    uint64_t getEpos() { return m_nEpos; }
};

class STT2DB {
private:
    static STT2DB* m_instance;
    
	bool m_bLiveFlag;

	std::queue< RTSTTQueItem* > m_qRtSttQue;
	std::thread m_thrd;
	mutable std::mutex m_mxQue;
	mutable std::mutex m_mxDb;
    
    URL_T m_url;
    ConnectionPool_T m_pool;

    log4cpp::Category *m_Logger;
    
private:
    STT2DB(log4cpp::Category *logger);
	static void thrdMain(STT2DB* s2d);
	void insertRtSTTData(RTSTTQueItem* item);
    
public:
    virtual ~STT2DB();
    
    static STT2DB* instance(std::string dbtype, std::string dbhost, std::string dbport, std::string dbuser, std::string dbpw, std::string dbname, std::string charset, log4cpp::Category *logger);
    static void release();
    
    void setLogger(log4cpp::Category *logger) { m_Logger = logger; }
    
    // for Realtime Call Siganl
    // VRClient에서 사용되는 api이며 실시간 통화 시작 및 종료 시 사용된다.
    int searchCallInfo(std::string counselorcode);
    int insertCallInfo(std::string counselorcode, std::string callid);
    int updateCallInfo(std::string callid, bool end=false);
    int updateCallInfo(std::string counselorcode, std::string callid, bool end=false);
    void insertRtSTTData(uint32_t idx, std::string callid, uint8_t spkno, uint64_t spos, uint64_t epos, std::string stt);
    
    // for batch
    // Schd4DB 모듈에서 사용되는 api
    // 처리할 task가 등록되었는지 확인(search)하고
    // 신규 task에 대해 VFClient가 처리할 수 있도록 전달(get) 후 처리된 task에 대해서는 삭제(delete)한다.
    void insertBatchTask();
    int getBatchTask();
    void deleteBatchTask();

    // for Task working
    // VFClient에서 사용되는 api로서 작업 시작 전,
    // 작업 완료 후 아래의 api를 이용하여 해당 task에 대한 정보를 handling한다.
    int insertTaskInfo(std::string downloadPath, std::string filename, std::string callId);
    int updateTaskInfo(std::string callid, std::string counselorcode, char state);
    int searchTaskInfo(std::string downloadPath, std::string filename, std::string callId);
    int getTaskInfo(std::vector< JobInfoItem* > &v);

    void restartConnectionPool();
};