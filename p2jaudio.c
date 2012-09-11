/**

Author: Elias Vanderstuyft (Elias.vds[at]gmail.com)
        Parts (for jack) are based on the code of the sampler program called 'Specimen',
        and (for pulse-simple) on the code example 'parec-simple.c' on 'freedesktop.org'.
        For expansion using the standard pulse lib (so not pulse-simple),
        I suggest to look at the 'pavucontrol' source code as a guiding line.
Name: p2jaudio
Description: A simple daemon/tool to pipe audio of PulseAudio Source devices to Jack Output ports.
The name is chosen in accordance with the name of the program 'a2jmidi', where 'p' stands for 'pulse'.
It can be handy when recording from multiple soundcards at the same time,
however, each device will have its own latency, so realtime manipulation of the signal,
e.g. live performances, is not really recommended.

**/



#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <math.h>
#include <time.h>
#include <getopt.h>

#include <jack/jack.h>
#include <pulse/simple.h>
#include <pulse/error.h>


#define DEBUG 0


#define USE_BENCHMARK 1
#if (USE_BENCHMARK==1)
	#define PULSE_MAX_BUFFER_TIME 1.0
#endif

#define MAX_BUFFER_UNDERRUN_TIME_MULTIPLIER 2.0
#define MIN_BUFFER_UNDERRUN_AMOUNT 5

#define MIN_BENCHMARK_TIME 0.5
#define MAX_BENCHMARK_TIME 4.0




/* prototypes */

static int samplerateChange(jack_nframes_t r, void* arg);
static int periodSizeChange(jack_nframes_t b, void* arg);

static int jack_start(char* sourceName, int nChannels, char** channelNames);
static int jack_process(jack_nframes_t frames, void* arg);
static int jack_stop();
static void jack_shutdown(void* arg);

/** These ones should be replaced by the non-simple pulse lib **/
static void pulseServer_setProcessCallback(int (*cb)());
static void pulseServer_start();
static void pulseServer_run();
static void pulseServer_stop();

static int pulse_start(char* sourceName, int rate, int nChannels);
static int pulse_process();
static int pulse_stop();

int timeToPeriods(double time);
static int startProcess();
static int initBenchmark();
static int updateBenchmarkVariables(int side);
static int initBuffer();
static void clearUnderrunVariables();
static int updateUnderrunVariables(int side);
static int stopProcess();
static int softrestartProcess();
int restartProcess();

int start(char* srcName, int nChnls, char** chlNames);
int stop();

static int lockWaiter();
static int unlockWaiter();


/* file-global variables */

static jack_port_t**	ports;
static jack_client_t*	jackClient;

static int				rate;
static int				newRate;
static int				periodSize;
static int				newPeriodSize;

static char*			sourceName;
static int				nChannels;
static char**			channelNames;

static pa_simple*		pulseStream;

static float*			pulseBuffer;
static int				pulsePeriodSize;
static int				pulseMaxPeriods;
static int				pulseMaxPeriodSize;

static int				pulseBufferFrames;
static int				pulseBufferOffset;
static int				pulseMissedPeriods;

static double			pulseMaxBufferTime;

/* bufferUnderrunSide
	-1: no buffer underrun.
	0: jack buffer underrun.
	1: pulse buffer underrun.
*/
static int				bufferUnderrunSide;

/* benchmarkStatus
	-1: no benchmark started.
	0: benchmark running, without detecting extra latency.
	1: benchmark running and detecting extra latency.
	2: benchmark finished.
	3: benchmark approved to not be restart again.
*/
static int				benchmarkStatus = -1;

static int				bufferUnderrunAmount;
static double			bufferUnderrunLastTime;
static double			bufferUnderrunTotalTime;

static double			maxBufferUnderrunTimeInterval;

static int				benchmarkMinPeriods;
static int				benchmarkMaxPeriods;

static int				benchmarkPeriodCounter;
static int				benchmarkTotalPeriodCounter;
static int				benchmarkCountTo;
static int				benchmarkMaxMissedPeriods;

/* state
	-2: both sides not initialized yet.
	-1: process not initialized yet.
	0: everything initialized, but no signals of both sides received yet.
	1: received signal of jack but not of pulse in this state.
	2: received signal of pulse while in state 1, this is either the benchmarking state or the common operating state.
	In this way, with this 'handshake', we try to start syncronised.
*/
static int				state = -2;

/* todo
	-1: just proceed through the functions and callbacks as normal.
	0: soft-restart the process, by skipping benchmarking.
	1: restart the process.
	2: stop the process and quit.
*/
static int				todo = -1;
static pthread_mutex_t	todoMutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t	bufferMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t	pulseMutex = PTHREAD_MUTEX_INITIALIZER;


/* working together to stop CTS */
typedef jack_default_audio_sample_t jack_sample_t;



int imin(int a, int b) {
	return a < b ? a : b;
}
int imax(int a, int b) {
	return a > b ? a : b;
}

double getTime() {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + (double)tv.tv_usec / 1000000;
}



static int samplerateChange(jack_nframes_t r, void* arg)
{
	if (newRate != r) {
		newRate = r;
		printf ("Rate changed to %d.\n", r);
		return restartProcess();
	}
	return 0;
}

static int periodSizeChange(jack_nframes_t b, void* arg)
{
	if (newPeriodSize != b) {
		newPeriodSize = b;
		printf ("Period Size changed to %d.\n", b);
		return restartProcess();
	}
	return 0;
}


static int jack_start(char* sourceName, int nChannels, char** channelNames)
{
	printf ("Starting Jack...\n");
	
	if (state > -2) {
		printf ("Jack already started.\n");
		return 0;
	}

	char* instancename = strdup(sourceName);
	if ((jackClient = jack_client_open(instancename, JackNullOption, NULL)) == 0) {
		printf ("Failed to open new jack client: %s\n", instancename);
		return -1;
	}

	jack_set_process_callback(jackClient, jack_process, 0);
	jack_on_shutdown(jackClient, jack_shutdown, 0);

	if ((ports = malloc(sizeof(jack_port_t*) * nChannels)) == NULL) {
		printf ("Failed to allocate space for ports.\n");
		jack_client_close(jackClient);
		return -1;
	}
	
	int i;
	for (i = 0; i < nChannels; i++) {
		ports[i] =
			jack_port_register(jackClient, channelNames[i], JACK_DEFAULT_AUDIO_TYPE,
					JackPortIsOutput, 0);
	}

	rate = newRate = jack_get_sample_rate(jackClient);
	jack_set_sample_rate_callback(jackClient, samplerateChange, 0);

	periodSize = newPeriodSize = jack_get_buffer_size(jackClient);
	jack_set_buffer_size_callback(jackClient, periodSizeChange, 0);

	if (jack_activate(jackClient) != 0) {
		printf ("Failed to activate jack client.\n");
		jack_client_close(jackClient);
		return -1;
	}

	// Change state
	
	state = -1;
	printf ("Jack started.\n");
	
	return 0;
}

static int jack_process(jack_nframes_t frames, void* arg)
{
	pthread_mutex_lock(&todoMutex);
	if (todo > -1) {
		pthread_mutex_unlock(&todoMutex);
		return 0;
	}
	pthread_mutex_unlock(&todoMutex);
	
	pthread_mutex_lock(&bufferMutex);
	
	if (state == 0) {
		#if (DEBUG==1)
		printf ("Jack process: state increased to 1.\n");
		#endif
		state = 1;
	}
	
	if (state < 2) {
		pthread_mutex_unlock(&bufferMutex);
		return 0;
	}
	
	if (nChannels * frames != pulsePeriodSize) {
		#if (DEBUG==1)
		printf ("Failed assertion: (nChannels * frames = %d) != (pulsePeriodSize = %d)\n", nChannels * frames, pulsePeriodSize);
		#endif
		pthread_mutex_unlock(&bufferMutex);
		stop();
		return 0;
	}
	
	pulseMissedPeriods++;
	
	if (USE_BENCHMARK && benchmarkStatus < 3) {
		if (benchmarkStatus < 2)
			benchmarkStatus = imax(updateBenchmarkVariables(0), benchmarkStatus);
	
	} else {
		const int underrunStatus = updateUnderrunVariables(0);
		if (underrunStatus == -2) {
			pthread_mutex_unlock(&bufferMutex);
			stop();
			return 0;
		}
		
		int bufferIdx;
		if (pulseBufferFrames == 0) {
			bufferIdx = (pulseMaxPeriodSize + pulseBufferOffset - pulsePeriodSize) % pulseMaxPeriodSize;
		} else {
			bufferIdx = pulseBufferOffset;
			pulseBufferFrames -= pulsePeriodSize;
			pulseBufferOffset = (pulseBufferOffset + pulsePeriodSize) % pulseMaxPeriodSize;
			#if (DEBUG==1)
			printf ("Reading buffer and shifting pulseBufferOffset to %d, then pulseBufferFrames = %d.\n", pulseBufferOffset, pulseBufferFrames);
			#endif
		}
	
		if (pulseBufferOffset + pulsePeriodSize > pulseMaxPeriodSize) {
			#if (DEBUG==1)
			printf ("Failed assertion: (pulseBufferOffset + pulsePeriodSize = %d) > (pulseMaxPeriodSize = %d)\n", pulseBufferOffset + pulsePeriodSize, pulseMaxPeriodSize);
			#endif
			pthread_mutex_unlock(&bufferMutex);
			stop();
			return 0;
		}
		
		#if (DEBUG==1)
		printf ("Jack Process.\n");
		#endif
		
		jack_sample_t* chnls[nChannels];
		int i;
		for (i = 0; i < nChannels; i++)
			chnls[i] = (jack_sample_t*) jack_port_get_buffer(ports[i], frames);
		
		int j;
		for (j = 0; j < frames; j++)
			for (i = 0; i < nChannels; i++) {
				chnls[i][j] = pulseBuffer[bufferIdx];
				bufferIdx++;
			}
	}

	pthread_mutex_unlock(&bufferMutex);
	return 0;
}

static int jack_stop()
{
	printf ("Stopping Jack...\n");
	
	if (state <= -2) {
		printf ("Jack already stopped.\n");
		return 0;
	}
	
	jack_deactivate(jackClient);
	jack_client_close(jackClient);
	free(ports);
	
	// Change state
	
	state = -2;
	printf ("Jack stopped.\n");
	
	return 0;
}

static void jack_shutdown(void* arg)
{
	stop();
}


static void intHandler(int sig)
{
	stop();
}
static void setupInterrupts()
{
	signal(SIGINT, intHandler);
	signal(SIGTERM, intHandler);
	signal(SIGKILL, intHandler);
}

static pthread_t pulseServerThread;
static pthread_mutex_t pulseServerMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pulseServerMutex2 = PTHREAD_MUTEX_INITIALIZER;
static int pulseRunning = -1;
static int (*pulseServer_processCb)();
static void pulseServer_setProcessCallback(int (*cb)()) {
	pulseServer_processCb = cb;
}
static void pulseServer_start()
{
	pthread_mutex_lock(&pulseServerMutex);
	
	for (;;) {
		if (pulseRunning == 1)
			break;
		
		if (pulseRunning == 0) {
			pthread_mutex_lock(&pulseServerMutex2);
			pthread_mutex_unlock(&pulseServerMutex);
			pthread_join(pulseServerThread, NULL);
			pthread_mutex_lock(&pulseServerMutex);
			pthread_mutex_unlock(&pulseServerMutex2);
		}
		
		if (pulseRunning == -1) {
			pthread_create(&pulseServerThread, NULL, (void*)pulseServer_run, NULL);
			break;
		}
	}
	
	pthread_mutex_unlock(&pulseServerMutex);
}
static void pulseServer_run()
{
	pthread_mutex_lock(&pulseServerMutex);
	pulseRunning = 1;
	pthread_mutex_unlock(&pulseServerMutex);
	
	printf ("Pulse Server has started.\n");
	
	while (pulseRunning) {
		(*pulseServer_processCb)();
	}
	
	printf ("Pulse Server has ended.\n");
	
	pthread_mutex_lock(&pulseServerMutex);
	pulseRunning = -1;
	pthread_mutex_unlock(&pulseServerMutex);
}
static void pulseServer_stop()
{
	pthread_mutex_lock(&pulseServerMutex);
	pulseRunning = 0;
	pthread_mutex_unlock(&pulseServerMutex);
}

static int pulse_start(char* sourceName, int rate, int nChannels)
{
	pthread_mutex_lock(&pulseMutex);
	printf ("Starting Pulse (%d*%dHz)...\n", nChannels, rate);
	
	if (state != -1) {
		#if (DEBUG==1)
		printf ("Pulse start: (state = %d) != -1.\n", state);
		#endif
		pthread_mutex_unlock(&pulseMutex);
		return -1;
	}
	
	/* The sample type to use */
	const pa_sample_spec ss = {
		.format = PA_SAMPLE_FLOAT32LE,
		.rate = rate,
		.channels = nChannels
	};

	/* Create the recording stream */
	int error;
	if (!(pulseStream = pa_simple_new(NULL, sourceName, PA_STREAM_RECORD, NULL, "record", &ss, NULL, NULL, &error))) {
		fprintf(stderr, __FILE__": pa_simple_new() failed: %s\n", pa_strerror(error));
		pthread_mutex_unlock(&pulseMutex);
		return -1;
	}
	
	pulseServer_setProcessCallback(pulse_process);
	pulseServer_start();
	printf ("Pulse started.\n");
	
	pthread_mutex_unlock(&pulseMutex);
	return 0;
}

static int pulse_process()
{
	pthread_mutex_lock(&todoMutex);
	if (todo > -1) {
		pthread_mutex_unlock(&todoMutex);
		return 0;
	}
	pthread_mutex_unlock(&todoMutex);
	
	pthread_mutex_lock(&pulseMutex);
	
	if (state < 0) {
		pthread_mutex_unlock(&pulseMutex);
		return -1;
	}
	
	float tmpBuffer[pulsePeriodSize];

	/* Record some data ... */
	int error;
	if (pa_simple_read(pulseStream, tmpBuffer, sizeof(tmpBuffer), &error) < 0) {
		fprintf(stderr, __FILE__": pa_simple_read() failed: %s\n", pa_strerror(error));
		pthread_mutex_unlock(&pulseMutex);
		stop();
		return -1;
	}
	
	pthread_mutex_unlock(&pulseMutex);
	
	pthread_mutex_lock(&bufferMutex);
	
	if (state < 1) {
		pthread_mutex_unlock(&bufferMutex);
		return -1;
	} else if (state == 1) {
		if (USE_BENCHMARK && benchmarkStatus == -1)
			initBenchmark();
		
		// Change state
		state = 2;
		#if (DEBUG==1)
		printf ("Pulse process: state increased to 2.\n");
		#endif
	}
	
	pulseMissedPeriods--;
	
	if (USE_BENCHMARK && benchmarkStatus < 3) {
		if (benchmarkStatus < 2)
			benchmarkStatus = imax(updateBenchmarkVariables(1), benchmarkStatus);
	
	} else {
		const int underrunStatus = updateUnderrunVariables(1);
		if (underrunStatus == -2) {
			pthread_mutex_unlock(&bufferMutex);
			stop();
			return 0;
		} else if (underrunStatus == -1)
			pulseBufferFrames = 0;
		
		#if (DEBUG==1)
		printf ("Pulse Process.\n");
		#endif
		
		const int pulseBufferOffsetAtEnd = (pulseBufferOffset + pulseBufferFrames) % pulseMaxPeriodSize;
		memcpy(&pulseBuffer[pulseBufferOffsetAtEnd], &tmpBuffer[0], sizeof(tmpBuffer));
		if (pulseBufferFrames == pulseMaxPeriodSize)
			pulseBufferOffset = (pulseBufferOffset + pulseBufferFrames) % pulseMaxPeriodSize;
		else
			pulseBufferFrames += pulsePeriodSize;
		#if (DEBUG==1)
		printf ("Writing to buffer from %d with length %d.\n", pulseBufferOffsetAtEnd, (int)sizeof(tmpBuffer));
		#endif
	}
	
	pthread_mutex_unlock(&bufferMutex);
	return 0;
}

static int pulse_stop()
{
	pthread_mutex_lock(&pulseMutex);
	printf ("Stopping Pulse...\n");
	
	if (state < -1) {
		#if (DEBUG==1)
		printf ("Pulse start: (state = %d) < -1.\n", state);
		#endif
		pthread_mutex_unlock(&pulseMutex);
		return -1;
	}
	
	pulseServer_stop();
	
	if (pulseStream != NULL)
		pa_simple_free(pulseStream);
	
	printf ("Pulse stopped.\n");
	
	pthread_mutex_unlock(&pulseMutex);
	return 0;
}


int timeToPeriods(double time) {
	return (int)ceil(nChannels * rate * time / pulsePeriodSize);
}

double periodsToTime(int periods) {
	return periods * pulsePeriodSize / (double)(nChannels * rate);
}

static int startProcess()
{
	printf ("Starting Process (%d*%dHz buffered in %d frames/channel)...\n", nChannels, rate, periodSize);
	
	if (state > -1) {
		printf ("Process already started.\n");
		return 0;
	} else if (state < -1) {
		printf ("Jack not started yet.\n");
		return -1;
	}
	
	// Update buffer
	
	pthread_mutex_lock(&bufferMutex);

	pulsePeriodSize = nChannels * periodSize;
	
		pulseMissedPeriods = 0;
		if (USE_BENCHMARK == 0 || benchmarkStatus >= 2) {
			if (USE_BENCHMARK == 0)
				pulseMaxBufferTime = PULSE_MAX_BUFFER_TIME;
			
			const int bufferStatus = initBuffer();
			if (bufferStatus == -1) {
				pthread_mutex_unlock(&bufferMutex);
				stop();
				return -1;
			}
			
			benchmarkStatus = 3;
		}
	
	pthread_mutex_unlock(&bufferMutex);
	
	// Start pulse
	
	if (pulse_start(sourceName, rate, nChannels) == -1) {
		stop();
		return -1;
	}
	
	// Change state
	
	state = 0;
	printf ("Process started.\n");
	
	return 0;
}

static int initBenchmark() {
	printf ("Benchmark started.\n");
	
	benchmarkMinPeriods = timeToPeriods(MIN_BENCHMARK_TIME);
	benchmarkMaxPeriods = timeToPeriods(MAX_BENCHMARK_TIME);
	
	benchmarkPeriodCounter = 0;
	benchmarkTotalPeriodCounter = 0;
	benchmarkCountTo = benchmarkMinPeriods;
	benchmarkMaxMissedPeriods = 0;
	
	return 0;
}

static int updateBenchmarkVariables(int side) {
	if (benchmarkPeriodCounter >= benchmarkCountTo) {
		printf ("Benchmark ended: benchmarkMaxMissedPeriods ended with %d => \n\t latency of %fms; I'll use a buffer of %dperiods.\n", benchmarkMaxMissedPeriods, 1000*(periodsToTime(benchmarkMaxMissedPeriods)), imax(1.25 * 2*benchmarkMaxMissedPeriods, 1) + 1);
		pulseMaxBufferTime = periodsToTime(imax(/* 1.25 * */ 2*benchmarkMaxMissedPeriods, 1) + 1);
		softrestartProcess();
		return 2;
	}
	
	const int magnitude = (1 - 2*side) * pulseMissedPeriods;
	benchmarkPeriodCounter++;
	
	if (magnitude > benchmarkMaxMissedPeriods) {
		benchmarkMaxMissedPeriods = magnitude;
		benchmarkTotalPeriodCounter += benchmarkPeriodCounter;
		benchmarkPeriodCounter = 0;
		benchmarkCountTo = imin(2 * benchmarkMaxMissedPeriods, benchmarkMaxPeriods - benchmarkTotalPeriodCounter);
		return 1;
	}
	
	return 0;
}

static int initBuffer() {
	pulseMaxPeriods = imax(timeToPeriods(pulseMaxBufferTime), 1);
	pulseMaxPeriodSize = pulseMaxPeriods * pulsePeriodSize;
	#if (DEBUG==1)
	printf ("pulseMaxPeriodSize set to %d.\n", pulseMaxPeriodSize);
	#endif
	
	if ((pulseBuffer = malloc(sizeof(float) * pulseMaxPeriodSize)) == NULL) {
		printf ("Failed to allocate new buffer size = %dB.\n", (int)sizeof(float) * pulseMaxPeriodSize);
		return -1;
	}
	
	// Init variables
	
	pulseBufferFrames = 0;
	pulseBufferOffset = 0;
	
	clearUnderrunVariables();
	bufferUnderrunLastTime = 0;
	maxBufferUnderrunTimeInterval = pulseMaxBufferTime * MAX_BUFFER_UNDERRUN_TIME_MULTIPLIER;
	
	return 0;
}

static void clearUnderrunVariables() {
	bufferUnderrunSide = -1;
	bufferUnderrunAmount = 0;
	bufferUnderrunTotalTime = 0;
}

static int updateUnderrunVariables(int side) {
	const int multiplier = 1 - 2*side;
	const int magnitude = multiplier * pulseMissedPeriods;
	
	if ( (magnitude + pulseMaxPeriods / 2 >= 0) && 
			(bufferUnderrunSide == side) )
		clearUnderrunVariables();
	
	if (magnitude > pulseMaxPeriods) {
		if (magnitude > 2*pulseMaxPeriods) {
			#if (DEBUG==1)
			printf ("%d-side process: Exceeded two times buffersize. => Resetting some stuff.\n", side);
			#else
			printf ("Buffer underrun.\n"); // TODO: printf is actually not allowed in a realtime process-thread.
			#endif
			pulseMissedPeriods -= multiplier * pulseMaxPeriods;
			
			if (bufferUnderrunSide == -1)
				bufferUnderrunLastTime = getTime();
			else
				bufferUnderrunTotalTime += getTime() - bufferUnderrunLastTime;
			bufferUnderrunSide = 1 - side;
			bufferUnderrunAmount++;
			
			if ( (bufferUnderrunTotalTime / bufferUnderrunAmount <= maxBufferUnderrunTimeInterval) &&
						(bufferUnderrunAmount >= MIN_BUFFER_UNDERRUN_AMOUNT) ) {
				#if (DEBUG==1)
				printf ("%d-side process: Shutting down: Too frequent buffer underruns.\n", side);
				#else
				printf ("Shutting down: Too frequent buffer underruns.\n");
				#endif
				return -2;
			} else {
				#if (DEBUG==1)
				printf ("%d-side process: Not frequent enough buffer underruns to quit: Interval = %f; Amount = %d.\n", side, bufferUnderrunTotalTime / bufferUnderrunAmount, bufferUnderrunAmount);
				#endif
			}
			
			return -1;
		}
		
		#if (DEBUG==1)
		printf ("%d-side process: underrun: pulseMaxBufferTime should be %f.\n", side, periodsToTime(magnitude));
		#endif
		return 0;
	}
	
	return 1;
}

static int stopProcess()
{
	printf ("Process stopping...\n");
	
	if (state <= -1) {
		printf ("Process already stopped.\n");
		return 0;
	}
	
	// Change state
	
	const int prevState = state;
	state = -1;
	if (todo != 0)
		benchmarkStatus = -1;
	
	pthread_mutex_lock(&bufferMutex);
	if (prevState == 3) {
		
		// Free buffer
		if (pulseBuffer != NULL)
			free(pulseBuffer);
	
	}
	pthread_mutex_unlock(&bufferMutex);
	
	// Stop pulse
	
	pulse_stop();
	printf ("Process stopped.\n");
	
	return 0;
}


static int changeTodo(int newTodo) {
	pthread_mutex_lock(&todoMutex);
	todo = imax(newTodo, todo);
	#if (DEBUG==1)
	printf ("todo change: %d.\n", todo);
	#endif
	pthread_mutex_unlock(&todoMutex);
	unlockWaiter();
	return 0;
}

static int softrestartProcess() {
	return changeTodo(0);
}

int restartProcess() {
	return changeTodo(1);
}

int start(char* srcName, int nChnls, char** chlNames)
{
	sourceName = srcName;
	nChannels = nChnls;
	channelNames = chlNames;
	
	int ret = jack_start(sourceName, nChannels, channelNames);
	if (ret == 0)
		ret = startProcess();
	return ret;
}

int stop()
{
	return changeTodo(2);
}


static pthread_t interruptThread;

static int waitUnlocked = 1;
static pthread_mutex_t	waitMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t	waitUnlockMutex = PTHREAD_MUTEX_INITIALIZER;
static int lockWaiter() {
	pthread_mutex_lock(&waitMutex);
	pthread_mutex_lock(&waitUnlockMutex);
	waitUnlocked = 0;
	pthread_mutex_unlock(&waitUnlockMutex);
	return 0;
}
static int unlockWaiter() {
	int ret;
	pthread_mutex_lock(&waitUnlockMutex);
	if (waitUnlocked == 0) {
		pthread_mutex_unlock(&waitMutex);
		waitUnlocked = 1;
	} else
		ret = -1;
	pthread_mutex_unlock(&waitUnlockMutex);
	return ret;
}

int run(char* srcName, int nChnls, char** chlNames) {
	pthread_create(&interruptThread, NULL, (void*)setupInterrupts, NULL);
	pthread_join(interruptThread, NULL);
	
	int ret = start(srcName, nChnls, chlNames);
	if (ret == -1)
		stop();
	
	for (;;) {
		lockWaiter();
// 		usleep(20000);

		pthread_mutex_lock(&todoMutex);
		if (todo > -1) {
			pthread_mutex_unlock(&todoMutex);
			stopProcess();
			pthread_mutex_lock(&todoMutex);
		}
		if (todo == 0 || todo == 1) {
			#if (DEBUG==1)
			printf ("Got restart type %d.\n", todo);
			#endif
			todo = -1;
			pthread_mutex_unlock(&todoMutex);
			rate = newRate;
			periodSize = newPeriodSize;
			printf ("Restarting Process...\n");
			if ((ret = startProcess()) == -1)
				stop();
			else
				printf ("Process restarted.\n");
		} else if (todo == 2) {
			#if (DEBUG==1)
			printf ("Got stop.\n");
			#endif
			pthread_mutex_unlock(&todoMutex);
			jack_stop();
			break;
		}
		pthread_mutex_unlock(&todoMutex);
	}
	
	unlockWaiter();
	
	return ret;
}

static char srcName[256];
static int nChnls = 2;
static char **chlNames;
static int processCmdArguments(int argc, char **argv) {
	int doesUserNeedHelp = 0;
	
	strcpy(srcName, "");
	
	static struct option long_options[] = {
		{"help",     no_argument,       0, 'h'},
		{"name",     required_argument, 0, 'n'},
		{"channels", required_argument, 0, 'c'},
		{0, 0, 0, 0}
	};
	int c, option_index;
	
	while (c != -1) {
		c = getopt_long(argc, argv, "c:hn:", long_options, &option_index);
		switch (c) {
			case 'n':
				strcpy(srcName, optarg);
				break;
	
			case 'c':
				nChnls = atoi(optarg);
				if (nChnls <= 0) {
					printf ("NUM_CHANNELS must be a number greater than zero.\n");
					doesUserNeedHelp = 1;
				}
				break;
			
			case -1:
				break;
			
			default:
				doesUserNeedHelp = 1;
				break;
		}
	}
	
	if (optind < argc) {
		while (optind < argc)
			printf ("'%s': Non-option arguments are not allowed.\n", argv[optind++]);
		doesUserNeedHelp = 1;
	}
	
	if (doesUserNeedHelp) {
		printf (
"\
Usage: \t %s [-n NAME] [-c NUM_CHANNELS] \n\
\n\
p2jaudio v0.01-alpha. \n\
Makes a pipe from a PulseAudio Source device to \n\
NUM_CHANNELS Jack Output ports and gives it the name NAME. \n\
\n\
Options: \n\
\t -n, --name=NAME              specify the name of the pipe, to be used as jack and pulse client-name \n\
\t -c, --channels=NUM_CHANNELS  specify the amount (> 0) of audio channels, to be used from the PulseAudio Source device \n\
\t -h, --help                   prints this help-message \n\
Read the README for more help on this program. \n\
", 
				argv[0]);
		
		return -1;
	}
	else {
		int i;

		if (strlen(srcName) == 0)
			strcpy(srcName, "p2jaudio");
		else
			strcat(srcName, " (p2jaudio)");
		
		if ((chlNames = malloc(sizeof(char*) * nChnls)) == NULL) {
			printf ("Failed to allocate memory for channel names.\n");
			return -1;
		}
		if (nChnls == 1)
			chlNames[0] = "mono";
		else if (nChnls == 2) {
			chlNames[0] = "left";
			chlNames[1] = "right";
		} else {
			char chlName[8+3];
			for (i = 0; i < nChnls; i++) {
				sprintf(chlName, "channel %d", i+1);
				chlNames[i] = strdup(chlName);
			}
		}

		printf ("Using the following config: \n\t Name: %s \n\t Amount of Channels: %d\n", srcName, nChnls);
		#if (DEBUG==1)
		for (i = 0; i < nChnls; i++)
			printf ("%d: '%s'\n", i+1, chlNames[i]);
		#endif
		
		return 0;
	}
}


int main(int argc, char **argv) {
	if (processCmdArguments(argc, argv) == 0)
		return run(srcName, nChnls, chlNames);
	else
		return 0;
	
// 	char* chlNames[1] = {"mono"};
// 	return run("p2jaudio", 1, chlNames);
	
// 	char* chlNames[2] = {"left", "right"};
// 	return run("p2jaudio", 2, chlNames);
}
