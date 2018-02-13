﻿#pragma once

#include <stdint.h>
#include <string>
#include <vector>
#include <mutex>

#include <log4cpp/Category.hh>

class VRCManager;
class VDClient;

class VDCManager
{
	static VDCManager* ms_instance;

	std::vector< VDClient* > m_vClients;

	mutable std::mutex m_mxVec;
    
    VRCManager *m_vrcm;
    log4cpp::Category *m_Logger;

public:
	static VDCManager* instance(uint16_t tcount, uint16_t bport, uint16_t eport, VRCManager *vrcm, log4cpp::Category *logger);
	static void release();

	int16_t requestVDC(std::string& callid, uint8_t noc, std::vector< uint16_t > &vPorts);
	void removeVDC(std::string& callid);

	void outputVDCStat();

private:
	VDCManager(VRCManager *vrcm, log4cpp::Category *logger);
	virtual ~VDCManager();
};

