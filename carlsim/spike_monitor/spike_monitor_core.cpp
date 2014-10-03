#include <spike_monitor_core.h>

#include <snn.h>				// CARLsim private implementation
#include <snn_definitions.h>	// CARLSIM_ERROR, CARLSIM_INFO, ...

#include <algorithm>			// std::sort



// we aren't using namespace std so pay attention!
SpikeMonitorCore::SpikeMonitorCore(CpuSNN* snn, int monitorId, int grpId) {
	snn_ = snn;
	grpId_= grpId;
	monitorId_ = monitorId;
	nNeurons_ = -1;
	spikeFileId_ = NULL;
	recordSet_ = false;
	spkMonLastUpdated_ = 0;

	mode_ = AER;
	persistentData_ = false;

	needToWriteFileHeader_ = true;
	spikeFileSignature_ = 206661989;
	spikeFileVersion_ = 1.0f;

	// defer all unsafe operations to init function
	init();
}

void SpikeMonitorCore::init() {
	nNeurons_ = snn_->getGroupNumNeurons(grpId_);
	assert(nNeurons_>0);

	// spike vector will be a 2D structure that holds a list of spike times for each neuron in the group
	// so the first dimension is neuron ID, and each spkVector_[i] holds the list of spike times of that neuron
	// fix the first dimension of spike vector
	spkVector_.resize(nNeurons_);

	clear();

	// use CARLSIM_{ERROR|WARNING|etc} typesetting (const FILE*)
	fpInf_ = snn_->getLogFpInf();
	fpErr_ = snn_->getLogFpErr();
	fpDeb_ = snn_->getLogFpDeb();
	fpLog_ = snn_->getLogFpLog();
}

SpikeMonitorCore::~SpikeMonitorCore() {
	if (spikeFileId_!=NULL) {
		fclose(spikeFileId_);
		spikeFileId_ = NULL;
	}
}

// +++++ PUBLIC METHODS: +++++++++++++++++++++++++++++++++++++++++++++++//

void SpikeMonitorCore::clear() {
	assert(!isRecording());
	recordSet_ = false;
	startTime_ = -1;
	startTimeLast_ = -1;
	stopTime_ = -1;
	accumTime_ = 0;
	totalTime_ = -1;

	for (int i=0; i<nNeurons_; i++)
		spkVector_[i].clear();

	needToCalculateFiringRates_ = true;
	needToSortFiringRates_ = true;
	firingRates_.clear();
	firingRatesSorted_.clear();
	firingRates_.assign(nNeurons_,0);
	firingRatesSorted_.assign(nNeurons_,0);
}

float SpikeMonitorCore::getPopMeanFiringRate() {
	assert(!isRecording());

	if (totalTime_==0)
		return 0.0f;

	return getPopNumSpikes()*1000.0/(getRecordingTotalTime()*nNeurons_);
}

float SpikeMonitorCore::getPopStdFiringRate() {
	assert(!isRecording());

	if (totalTime_==0)
		return 0.0f;

	float meanRate = getPopMeanFiringRate();
	std::vector<float> rates = getAllFiringRates();
	float std = 0.0f;
	if (nNeurons_>1) {
		for (int i=0; i<nNeurons_; i++)
			std += (rates[i]-meanRate)*(rates[i]-meanRate);
		std = sqrt(std/(nNeurons_-1));
	}

	return std;
}

int SpikeMonitorCore::getPopNumSpikes() {
	assert(!isRecording());

	int nSpk = 0;
	for (int i=0; i<nNeurons_; i++)
		nSpk += getNeuronNumSpikes(i);

	return nSpk;
}

std::vector<float> SpikeMonitorCore::getAllFiringRates() {
	assert(!isRecording());

	// if necessary, get data structures up-to-date
	calculateFiringRates();

	return firingRates_;
}

float SpikeMonitorCore::getMaxFiringRate() {
	assert(!isRecording());

	std::vector<float> rates = getAllFiringRatesSorted();

	return rates.back();
}

float SpikeMonitorCore::getMinFiringRate(){
	assert(!isRecording());

	std::vector<float> rates = getAllFiringRatesSorted();

	return rates.front();
}

float SpikeMonitorCore::getNeuronMeanFiringRate(int neurId) {
	assert(!isRecording());
	assert(neurId>=0 && neurId<nNeurons_);

	return getNeuronNumSpikes(neurId)*1000.0/getRecordingTotalTime();	
}

int SpikeMonitorCore::getNeuronNumSpikes(int neurId) {
	assert(!isRecording());
	assert(neurId>=0 && neurId<nNeurons_);
	assert(getMode()==AER);

	return spkVector_[neurId].size();
}

std::vector<float> SpikeMonitorCore::getAllFiringRatesSorted() {
	assert(!isRecording());

	// if necessary, get data structures up-to-date
	sortFiringRates();

	return firingRatesSorted_;
}

int SpikeMonitorCore::getNumNeuronsWithFiringRate(float min, float max){
	assert(!isRecording());
	assert(min>=0.0f && max>=0.0f);
	assert(max>=min);

	// if necessary, get data structures up-to-date
	sortFiringRates();

	int counter = 0;
	std::vector<float>::const_iterator it_begin = firingRatesSorted_.begin();
	std::vector<float>::const_iterator it_end = firingRatesSorted_.end();
	for(std::vector<float>::const_iterator it=it_begin; it!=it_end; it++){
		if((*it) >= min && (*it) <= max)
			counter++;
	}

	return counter;
}

int SpikeMonitorCore::getNumSilentNeurons() {
	assert(!isRecording());

	return getNumNeuronsWithFiringRate(0.0f, 0.0f);
}

// \TODO need to do error check on interface
float SpikeMonitorCore::getPercentNeuronsWithFiringRate(float min, float max) {
	assert(!isRecording());

	return getNumNeuronsWithFiringRate(min,max)*100.0/nNeurons_;
}

float SpikeMonitorCore::getPercentSilentNeurons(){
	assert(!isRecording());

	return getNumNeuronsWithFiringRate(0,0)*100.0/nNeurons_;
}

std::vector<std::vector<int> > SpikeMonitorCore::getSpikeVector2D(){
	assert(!isRecording());
	assert(mode_==AER);

	return spkVector_;
}

void SpikeMonitorCore::print(bool printSpikeTimes) {
	assert(!isRecording());

	// how many spike times to display per row
	int dispSpkTimPerRow = 7;

	CARLSIM_INFO("(t=%.3fs) SpikeMonitor for group %s(%d) has %d spikes in %ld ms (%.2f +/- %.2f Hz)",
		(float)(snn_->getSimTime()/1000.0),
		snn_->getGroupName(grpId_,0).c_str(),
		grpId_,
		getPopNumSpikes(),
		getRecordingTotalTime(),
		getPopMeanFiringRate(),
		getPopStdFiringRate());

	if (printSpikeTimes && mode_==AER) {
		// spike times only available in AER mode
		CARLSIM_INFO("| Neur ID | Rate (Hz) | Spike Times (ms)");
		CARLSIM_INFO("|- - - - -|- - - - - -|- - - - - - - - - - - - - - - - -- - - - - - - - - - - - -")

		for (int i=0; i<nNeurons_; i++) {
			char buffer[200];
#if (WIN32 || WIN64)
			_snprintf(buffer, 200, "| %7d | % 9.2f | ", i, getNeuronMeanFiringRate(i));
#else
			snprintf(buffer, 200, "| %7d | % 9.2f | ", i, getNeuronMeanFiringRate(i));
#endif
			int nSpk = spkVector_[i].size();
			for (int j=0; j<nSpk; j++) {
				char times[5];
#if (WIN32 || WIN64)
				_snprintf(times, 10, "%8d", spkVector_[i][j]);
#else
				snprintf(times, 10, "%8d", spkVector_[i][j]);
#endif
				strcat(buffer, times);
				if (j%dispSpkTimPerRow == dispSpkTimPerRow-1 && j<nSpk-1) {
					CARLSIM_INFO("%s",buffer);
					strcpy(buffer,"|         |           | ");
				}
			}
			CARLSIM_INFO("%s",buffer);
		}
	}
}

void SpikeMonitorCore::pushAER(int time, int neurId) {
	assert(isRecording());
	assert(getMode()==AER);

	spkVector_[neurId].push_back(time);
}

void SpikeMonitorCore::startRecording() {
	assert(!isRecording());

	if (!persistentData_) {
		// if persistent mode is off (default behavior), automatically call clear() here
		clear();
	}

	// call updateSpikeMonitor to make sure spike file and spike vector are up-to-date
	// Caution: must be called before recordSet_ is set to true!
	snn_->updateSpikeMonitor(grpId_);

	needToCalculateFiringRates_ = true;
	needToSortFiringRates_ = true;
	recordSet_ = true;
	long int currentTime = snn_->getSimTimeSec()*1000+snn_->getSimTimeMs();

	if (persistentData_) {
		// persistent mode on: accumulate all times
		// change start time only if this is the first time running it
		startTime_ = (startTime_<0) ? currentTime : startTime_;
		startTimeLast_ = currentTime;
		accumTime_ = (totalTime_>0) ? totalTime_ : 0;
	}
	else {
		// persistent mode off: we only care about the last probe
		startTime_ = currentTime;
		startTimeLast_ = currentTime;
		accumTime_ = 0;
	}
}

void SpikeMonitorCore::stopRecording() {
	assert(isRecording());
	assert(startTime_>-1 && startTimeLast_>-1 && accumTime_>-1);

	// call updateSpikeMonitor to make sure spike file and spike vector are up-to-date
	// Caution: must be called before recordSet_ is set to false!
	snn_->updateSpikeMonitor(grpId_);

	recordSet_ = false;
	stopTime_ = snn_->getSimTimeSec()*1000+snn_->getSimTimeMs();

	// total time is the amount of time of the last probe plus all accumulated time from previous probes
	totalTime_ = stopTime_-startTimeLast_ + accumTime_;
	assert(totalTime_>=0);
}

void SpikeMonitorCore::setSpikeFileId(FILE* spikeFileId) {
	assert(!isRecording());

	// \TODO consider the case where this function is called more than once
	if (spikeFileId_!=NULL)
		CARLSIM_ERROR("SpikeMonitorCore: setSpikeFileId has already been called.");

	spikeFileId_=spikeFileId;

	// for now: file pointer has changed, so we need to write header (again)
	needToWriteFileHeader_ = true;
	writeSpikeFileHeader();
}


// calculate average firing rate for every neuron if we haven't done so already
void SpikeMonitorCore::calculateFiringRates() {
	// only update if we have to
	if (!needToCalculateFiringRates_)
		return;

	assert(getMode()==AER);

	// clear, so we get the same answer every time.
	firingRates_.assign(nNeurons_,0);
	firingRatesSorted_.assign(nNeurons_,0);

	// this really shouldn't happen at this stage, but if recording time is zero, return all zeros
	if (totalTime_==0) {
		CARLSIM_WARN("SpikeMonitorCore: calculateFiringRates has 0 totalTime");
		return;
	}

	// compute firing rate
	assert(totalTime_>0); // avoid division by zero
	for(int i=0;i<nNeurons_;i++) {
		firingRates_[i]=spkVector_[i].size()*1000.0/totalTime_;
	}

	needToCalculateFiringRates_ = false;
}

// sort firing rates if we haven't done so already
void SpikeMonitorCore::sortFiringRates() {
	// only sort if we have to
	if (!needToSortFiringRates_)
		return;

	// first make sure firing rate vector is up-to-date
	calculateFiringRates();

	firingRatesSorted_=firingRates_;
	std::sort(firingRatesSorted_.begin(),firingRatesSorted_.end());

	needToSortFiringRates_ = false;
}

// write the header section of the spike file
// this should be done once per file, and should be the very first entries in the file
void SpikeMonitorCore::writeSpikeFileHeader() {
	if (!needToWriteFileHeader_)
		return;

	// write file signature
	if (!fwrite(&spikeFileSignature_,sizeof(int),1,spikeFileId_))
		CARLSIM_ERROR("SpikeMonitorCore: writeSpikeFileHeader has fwrite error");

	// write version number
	if (!fwrite(&spikeFileVersion_,sizeof(int),1,spikeFileId_))
		CARLSIM_ERROR("SpikeMonitorCore: writeSpikeFileHeader has fwrite error");

	needToWriteFileHeader_ = false;
}