// poll2_core.cpp

#include <algorithm>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <string.h>
#include <sstream>
#include <ctime>

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include "poll2_core.h"

// Interface for the PIXIE-16
#include "Utility.h"
#include "Buffer_Structure.h"
#include "StatsHandler.hpp"
#include "Display.h"

#define MAXIMUM_NUM_COMMANDS 50

// Values associated with the minimum timing between pixie calls (in us)
// Adjusted to help alleviate the issue with data corruption
#define END_RUN_PAUSE 100
#define POLL_PAUSE 0
#define READ_PAUSE 0
#define WAIT_PAUSE 0
#define POLL_TRIES 100
#define WAIT_TRIES 100

Poll::Poll(){
	// Maximum size of the shared memory buffer
	MAX_SHM_SIZEL = 4050; // in pixie words
	MAX_SHM_SIZE = MAX_SHM_SIZEL * sizeof(word_t); // in bytes
	CLOCK_VSN = 1000;

	// System flags and variables
	SYS_MESSAGE_HEAD = " POLL: ";
	KILL_ALL = false; // Set to true when the program is exiting
	START_RUN = false; // Set to true when the command is given to start a run
	STOP_RUN = false; // Set to true when the command is given to stop a run
	DO_REBOOT = false; // Set to true when the user tells POLL to reboot PIXIE
	FORCE_SPILL = false; // Force poll2 to dump the current data spill
	POLL_RUNNING = false; // Set to true when run_command is recieving data from PIXIE
	RUN_CTRL_EXIT = false; // Set to true when run_command exits
	RAW_TIME = 0;

	// Run control variables
	BOOT_FAST = false;
	INSERT_WALL_CLOCK = true;
	IS_QUIET = false;
	SEND_ALARM = false;
	SHOW_MODULE_RATES = false;
	ZERO_CLOCKS = false;
	DEBUG_MODE = false;
	SHM_MODE = true;

	// Options relating to output data file
	OUTPUT_DIRECTORY = "./"; // Set with 'fdir' command
	OUTPUT_FILENAME = "pixie"; // Set with 'ouf' command
	OUTPUT_TITLE = "PIXIE data file"; // Set with 'htit' command
	OUTPUT_RUN_NUM = 0; // Set with 'hnum' command
	OUTPUT_FORMAT = 0; // Set with 'oform' command

	// The main output data file and related variables
	CURRENT_FILE_NUM = 0;
	CURRENT_FILENAME = "";
	
	STATS_INTERVAL = -1; //< in seconds
	HISTO_INTERVAL = -1; //< in seconds

	runDone = NULL;
	fifoData = NULL;
	partialEventData = NULL;
	statsHandler = NULL;
}

Poll::~Poll(){
	restore_terminal();
	if(runDone){ delete[] runDone; }
	if(fifoData){ delete[] fifoData; }
	if(partialEventData){ delete[] partialEventData; }
}

bool Poll::initialize(){
	// Set debug mode
	if(DEBUG_MODE){ 
		std::cout << SYS_MESSAGE_HEAD << "Setting debug mode\n";
		OUTPUT_FILE.SetDebugMode(); 
	}

	// Initialize the pixie interface and boot
	pif.GetSlots();
	if(!pif.Init()){ return false; }

	if(BOOT_FAST){
		if(!pif.Boot(PixieInterface::DownloadParameters | PixieInterface::SetDAC | PixieInterface::ProgramFPGA)){ return false; } 
	}
	else{
		if(!pif.Boot(PixieInterface::BootAll)){ return false; }
	}

	// Check the scheduler
	Display::LeaderPrint("Checking scheduler");
	int startScheduler = sched_getscheduler(0);
	if(startScheduler == SCHED_BATCH){ std::cout << Display::InfoStr("BATCH") << std::endl; }
	else if(startScheduler == SCHED_OTHER){ std::cout << Display::InfoStr("STANDARD") << std::endl; }
	else{ std::cout << Display::WarningStr("UNEXPECTED") << std::endl; }

	if(!synch_mods(pif)){ return EXIT_FAILURE; }

	// Allocate memory buffers for FIFO
	N_CARDS = pif.GetNumberCards();
	
	// Two extra words to store size of data block and module number
	std::cout << "\nAllocating memory to store FIFO data (" << sizeof(word_t) * (EXTERNAL_FIFO_LENGTH + 2) * N_CARDS / 1024 << " kB)" << std::endl;
	fifoData = new word_t[(EXTERNAL_FIFO_LENGTH + 2) * N_CARDS];
	
	// Allocate data for partial events
	std::cout << "Allocating memory for partial events (" << sizeof(eventdata_t) * N_CARDS / 1024 << " kB)" << std::endl;
	partialEventData = new eventdata_t[N_CARDS];
	for(size_t card = 0; card < N_CARDS; card++){
		partialEventWords.push_back(0);
		waitWords.push_back(0);
	}

	dataWords = 0;
	statsTime = 0;
	histoTime = 0;

	if(HISTO_INTERVAL != -1.){ 
		std::cout << "Allocating memory to store HISTOGRAM data (" << sizeof(PixieInterface::Histogram)*N_CARDS*pif.GetNumberChannels()/1024 << " kB)" << std::endl;
		for (unsigned int mod=0; mod < N_CARDS; mod++){
			for (unsigned int ch=0; ch < pif.GetNumberChannels(); ch++){
			  chanid_t id(mod, ch);
			  histoMap[id] = PixieInterface::Histogram();
			}
		}
	}

	runDone = new bool[N_CARDS];
	isExiting = false;

	waitCounter = 0;
	nonWaitCounter = 0;
	partialBufferCounter = 0;
	
	return true;
}

/* Reset the terminal attributes to normal. */
void Poll::restore_terminal(){
	tcsetattr(STDIN_FILENO, TCSANOW, &SAVED_ATTRIBUTES);
}

/* Take control of the terminal to capture user input. */
bool Poll::takeover_terminal(){
	struct termios tattr;
	char *name;

	/* Make sure stdin is a terminal. */
	if(!isatty (STDIN_FILENO)){
		fprintf (stderr, "Not a terminal.\n");
		return false;
	}

	/* Save the terminal attributes so we can restore them later. */
	tcgetattr(STDIN_FILENO, &SAVED_ATTRIBUTES);

	/* Set the terminal modes. */
	tcgetattr(STDIN_FILENO, &tattr);
	tattr.c_cc[VMIN] = 1;
	tattr.c_cc[VTIME] = 0;
	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &tattr) == 0){ return true;  }	
	return false;
}

/* Safely close current data file if one is open. */
bool Poll::close_output_file(){
	if(OUTPUT_FILE.IsOpen()){ // A file is already open and must be closed
		if(POLL_RUNNING){
			std::cout << SYS_MESSAGE_HEAD << "Warning! attempted to close file while acquisition running.\n";
			return false;
		}
		else if(START_RUN){
			std::cout << SYS_MESSAGE_HEAD << "Warning! attempted to close file while acquisition is starting.\n";
			return false;		
		}
		else if(STOP_RUN){
			std::cout << SYS_MESSAGE_HEAD << "Warning! attempted to close file while acquisition is stopping.\n";
			return false;			
		}
		std::cout << SYS_MESSAGE_HEAD << "Closing output file.\n";
		OUTPUT_FILE.CloseFile();
		return true;
	}
	std::cout << SYS_MESSAGE_HEAD << "No file is open.\n";
	return true;
}

/* Print help dialogue for POLL options. */
void Poll::help(){
	std::cout << "  Help:\n";
	std::cout << "   quit           - Close the program\n";
	std::cout << "   help (h)       - Display this dialogue\n";
	std::cout << "   version (v)    - Display Poll2 version information\n";
	std::cout << "   status         - Display system status information\n";
	std::cout << "   run (trun)     - Start recording data to disk\n";
	std::cout << "   stop (tstop)   - Stop recording data to disk\n";
	std::cout << "   reboot         - Reboot PIXIE crate\n";
	std::cout << "   force          - Force dump of current spill\n";
	std::cout << "   debug			- Toggle debug mode flag\n";
	std::cout << "   fdir [path]    - Set the output file directory (default='./')\n";
	std::cout << "   ouf [filename] - Set the output filename (default='pixie.xxx')\n";
	std::cout << "   clo            - Safely close the current data output file\n";
	std::cout << "   htit [title]   - Set the title of the current run (default='PIXIE Data File)\n";
	std::cout << "   hnum [number]  - Set the number of the current run (default=0)\n";
	std::cout << "   oform [0,1,2]  - Set the format of the output file (default=0)\n\n";

	/* Future commands (exciting)
	std::cout << "   mca [filename='MCA.root'] [time=60s] - Use MCA to record data for debugging purposes\n";
	std::cout << "   pread [mod] [chan] [param]           - Read parameters from individual PIXIE channels\n";
	std::cout << "   pmread [mod] [param]                 - Read parameters from PIXIE modules\n";
	std::cout << "   pwrite [mod] [chan] [param] [val]    - Write parameters to individual PIXIE channels\n";
	std::cout << "   pmwrite [mod] [param] [val]          - Write parameters to PIXIE modules\n";*/
}

bool Poll::synch_mods(PixieInterface &pif){
	static bool firstTime = true;
	static char synchString[] = "IN_SYNCH";
	static char waitString[] = "SYNCH_WAIT";

	bool hadError = false;
	Display::LeaderPrint("Synchronizing");

	if(firstTime){
		// only need to set this in the first module once
		if(!pif.WriteSglModPar(waitString, 1, 0)){ hadError = true; }
		firstTime = false;
	}
	
	for(unsigned int mod = 0; mod < pif.GetNumberCards(); mod++){
		if (!pif.WriteSglModPar(synchString, 0, mod)){ hadError = true; }
	}

	if (!hadError){ std::cout << Display::OkayStr() << std::endl; }
	else{ std::cout << Display::ErrorStr() << std::endl; }

	return !hadError;
}

int Poll::write_data(word_t *data, unsigned int nWords, bool shm_/*=false*/){
	static data_pack acqBuf;
	const unsigned int sendPause = 1;

	if(OUTPUT_FORMAT == 0){ // legacy .ldf format		
		// Handle the writing of buffers to the file
		return OUTPUT_FILE.Write((char*)data, nWords);
		
		/*word_t *pWrite = (word_t *)acqBuf.Data; // Now write to the shared memory
		unsigned int nBufs = nWords / MAX_SHM_SIZEL;
		unsigned int wordsLeft = nWords % MAX_SHM_SIZEL;
		unsigned int totalBufs = nBufs + 1 + ((wordsLeft != 0) ? 1 : 0);

		for(size_t buf=0; buf < nBufs; buf++){
			OUTPUT_FILE.write((char*)&bufftype, 4);
			OUTPUT_FILE.write((char*)&buffsize, 4);
			
			acqBuf.BufLen = (MAX_SHM_SIZEL + 3) * sizeof(word_t);
			pWrite[0] = acqBuf.BufLen; // size
			pWrite[1] = totalBufs; // number of buffers we expect
			pWrite[2] = buf; 
			memcpy(&pWrite[3], &data[buf * MAX_SHM_SIZEL], MAX_SHM_SIZE); 

			if(OUTPUT_FILE.is_open()){ OUTPUT_FILE.write(acqBuf.Data, 16212); } // MAX_SHM_SIZE + 3 * sizeof(word_t)
			if(shm_){ send_buf(&acqBuf); }
			usleep(sendPause);
		}

		// send the last fragment (if there is any)
		if (wordsLeft != 0) {
			OUTPUT_FILE.write((char*)&bufftype, 4);
			OUTPUT_FILE.write((char*)&buffsize, 4);
		
			acqBuf.BufLen = (wordsLeft + 3) * sizeof(word_t);
			pWrite[0] = acqBuf.BufLen;
			pWrite[1] = totalBufs;
			pWrite[2] = nBufs;
			memcpy(&pWrite[3], &data[nBufs * MAX_SHM_SIZEL], wordsLeft * sizeof(word_t) );

			if(OUTPUT_FILE.is_open()){ OUTPUT_FILE.write(acqBuf.Data, wordsLeft * sizeof(word_t) + 12); }

			if(shm_){ send_buf(&acqBuf); }
			usleep(sendPause);
		}

		// send a buffer to say that we are done
		acqBuf.BufLen = 5 * sizeof(word_t);
		pWrite[0] = /;
		pWrite[1] = totalBufs;
		pWrite[2] = totalBufs - 1;
		
		// pacman looks for the following data
		pWrite[3] = 0x2; 
		pWrite[4] = 0x270f; // vsn 9999

		if(OUTPUT_FILE.is_open()){ OUTPUT_FILE.write((char*)pWrite, 5 * sizeof(word_t)); }

		if(shm_){ send_buf(&acqBuf); }*/
	}
	else if(OUTPUT_FORMAT == 1){ // new .pldf format
		
	}
	else if(OUTPUT_FORMAT == 2){
	}

	return -1;
}

/* Function to control the POLL command line interface */
void Poll::command_control(){
	char c;
	bool cmd_ready = false;
	std::string cmd = "";
	std::queue<std::string> old_commands; // FIFO command history storage
	
	while(true){
		read(STDIN_FILENO, &c, 1);
		
		// check for system control commands
		if(c == '\004'){ break; } // ctrl + c
		else if(c == '\n' || c == '\r'){
			cmd_ready = true;
		}
		else if(c == '\033'){
			read(STDIN_FILENO, &c, 1); // skip the '['
			read(STDIN_FILENO, &c, 1);
			if(c == 'A'){ std::cout << "up\n"; }
			else if(c == 'B'){ std::cout << "down\n"; }
		}
		else{ cmd += c; }
		
		if(cmd_ready){
			old_commands.push(cmd);
			if(old_commands.size() > MAXIMUM_NUM_COMMANDS){ old_commands.pop(); }
			
			if(cmd == ""){ continue; }
			
			// check for defined commands
			if(cmd == "quit"){
				if(POLL_RUNNING){ std::cout << SYS_MESSAGE_HEAD << "Warning! cannot quit while acquisition running\n"; }
				else{ 
					KILL_ALL = true;
					while(!RUN_CTRL_EXIT){ sleep(1); }
					break;
				}
			}
			else if(cmd == "kill"){
				if(POLL_RUNNING){ 
					std::cout << SYS_MESSAGE_HEAD << "Sending KILL signal\n";
					STOP_RUN = true; 
				}
				KILL_ALL = true;
				while(!RUN_CTRL_EXIT){ sleep(1); }
				break;
			}
			else if(cmd == "help" || cmd == "h"){ help(); }
			else if(cmd == "version" || cmd == "v"){ std::cout << "  Poll v" << POLL_VERSION << std::endl; }
			else if(cmd == "status"){
				std::cout << "  Poll Status:\n";
				std::cout << "   Run starting - " << yesno(START_RUN) << std::endl;
				std::cout << "   Run stopping - " << yesno(STOP_RUN) << std::endl;
				std::cout << "   Rebooting    - " << yesno(DO_REBOOT) << std::endl;
				std::cout << "   Acq running  - " << yesno(POLL_RUNNING) << std::endl << std::endl;
			}
			else if(cmd == "trun" || cmd == "run"){ START_RUN = true; } // Tell POLL to start taking data
			else if(cmd == "tstop" || cmd == "stop"){ STOP_RUN = true; } // Tell POLL to stop taking data
			else if(cmd == "reboot"){ // Tell POLL to attempt a PIXIE crate reboot
				if(POLL_RUNNING){ std::cout << SYS_MESSAGE_HEAD << "Warning! cannot quit while acquisition running\n"; }
				else{ DO_REBOOT = true; }
			}
			else if(cmd == "clo" || cmd == "close"){ close_output_file(); } // Tell POLL to close the current data file
			else if(cmd == "hup" || cmd == "force"){ FORCE_SPILL = true; } // Force spill write to file
			else if(cmd == "debug"){ // Toggle debug mode
				if(DEBUG_MODE){
					std::cout << SYS_MESSAGE_HEAD << "Toggling debug mode OFF\n";
					OUTPUT_FILE.SetDebugMode(false);
					DEBUG_MODE = false;
				}
				else{
					std::cout << SYS_MESSAGE_HEAD << "Toggling debug mode ON\n";
					OUTPUT_FILE.SetDebugMode();
					DEBUG_MODE = true;
				}
			}
			else if(cmd == "test"){
				if(!OUTPUT_FILE.IsOpen()){
					std::cout << SYS_MESSAGE_HEAD << "Performing write test\n";
					std::string temp;
					OUTPUT_FILE.OpenNewFile("This is a test file!", 1, temp);
					int test_data[13141];
					for(unsigned int i = 0; i < 13141; i++){ test_data[i] = (int)i; }
					OUTPUT_FILE.Write((char*)test_data, 13141);
					std::cout << SYS_MESSAGE_HEAD << "Wrote test file '" << temp << "'.\n";
					OUTPUT_FILE.CloseFile();
				}
				else{ std::cout << SYS_MESSAGE_HEAD << "Failed to perform write test (output file is open)\n"; }			
			}
			else if(cmd.find(" ") != std::string::npos){ // Commands with arguments
				size_t index = cmd.find(" ");
				std::string cmd2 = cmd.substr(0, index); // Get the command from the full input string
				std::string arg = cmd.substr(index+1, cmd.size()-index); // Get the argument from the full input string
				
				if(cmd2 == "fdir"){ // Change the output file directory
					OUTPUT_DIRECTORY = arg; 
					CURRENT_FILE_NUM = 0;
					std::cout << SYS_MESSAGE_HEAD << "Set output directory to '" << OUTPUT_DIRECTORY << "'\n";
				} 
				else if(cmd2 == "ouf"){ // Change the output file name
					OUTPUT_FILENAME = arg; 
					CURRENT_FILE_NUM = 0;
					OUTPUT_FILE.SetFilenamePrefix(OUTPUT_FILENAME);
					std::cout << SYS_MESSAGE_HEAD << "Set output filename to '" << OUTPUT_FILENAME << "'\n";
				} 
				else if(cmd2 == "htit"){ // Change the title of the output file
					OUTPUT_TITLE = arg; 
					std::cout << SYS_MESSAGE_HEAD << "Set run title to '" << OUTPUT_TITLE << "'\n";
				} 
				else if(cmd2 == "hnum"){ // Tell POLL to attempt a PIXIE crate reboot
					OUTPUT_RUN_NUM = atoi(arg.c_str()); 
					std::cout << SYS_MESSAGE_HEAD << "Set run number to '" << OUTPUT_RUN_NUM << "'\n";
				} 
				else if(cmd2 == "oform"){ // Change the output file format
					int format = atoi(arg.c_str());
					if(format == 0 || format == 1 || format == 2){
						OUTPUT_FORMAT = atoi(arg.c_str());
						std::cout << SYS_MESSAGE_HEAD << "Set output file format to '" << OUTPUT_FORMAT << "'\n";
						if(OUTPUT_FORMAT == 1){ std::cout << "  Warning! this output format is experimental and is not recommended\n"; }
						else if(OUTPUT_FORMAT == 2){ std::cout << "  Warning! this output format is slow and should only be used for debugging/troubleshooting\n"; }
						OUTPUT_FILE.SetFileFormat(OUTPUT_FORMAT);
					}
					else{ 
						std::cout << SYS_MESSAGE_HEAD << "Unknown output file format ID '" << format << "'\n";
						std::cout << "  Available file formats include:\n";
						std::cout << "   0 - .ldf (HRIBF) file format (default)\n";
						std::cout << "   1 - .pld (PIXIE) file format (experimental)\n";
						std::cout << "   2 - .root file format (slow, not recommended)\n";
					}
				}
				else{ std::cout << SYS_MESSAGE_HEAD << "Unknown command '" << cmd2 << "'\n"; }
			}
			else{ std::cout << SYS_MESSAGE_HEAD << "Unknown command '" << cmd << "'\n"; }

			cmd = "";
			cmd_ready = false;
		}
	}
}

/* Function to control the gathering and recording of PIXIE data */
void Poll::run_control(){
	std::vector<word_t> nWords(N_CARDS);
	std::vector<word_t>::iterator maxWords;
	parseTime = waitTime = readTime = 0.;

	while(true){
		if(KILL_ALL){ // Supercedes all other commands
			if(POLL_RUNNING){ STOP_RUN = true; }
			else{ break; }
		}
		
		if(DO_REBOOT){ // Attempt to reboot the PIXIE crate
			if(POLL_RUNNING){ STOP_RUN = true; }
			else{
				std::cout << SYS_MESSAGE_HEAD << "Attempting PIXIE crate reboot\n";
				pif.Boot(PixieInterface::BootAll);
				DO_REBOOT = false;
			}
		}

		// MAIN DATA ACQUISITION SECTION!!!
		if(POLL_RUNNING){
			if(START_RUN){
				std::cout << SYS_MESSAGE_HEAD << "Already running!\n";
				START_RUN = false;
			}
			
			// Data acquisition
		    // Check if it's time to dump statistics
			if(STATS_INTERVAL != -1 && usGetTime(startTime) > lastStatsTime + STATS_INTERVAL * 1e6){
				usGetDTime(); // start timer
				for (size_t mod = 0; mod < N_CARDS; mod++) {
					pif.GetStatistics(mod);
					PixieInterface::stats_t &stats = pif.GetStatisticsData();
					fifoData[dataWords++] = PixieInterface::STAT_SIZE + 3;
					fifoData[dataWords++] = mod;
					fifoData[dataWords++] = ((PixieInterface::STAT_SIZE + 1) << 17 ) & ( 1 << 12 ) & ( pif.GetSlotNumber(mod) << 4);
					memcpy(&fifoData[dataWords], &stats, sizeof(stats));
					dataWords += PixieInterface::STAT_SIZE;

					if(IS_QUIET){ std::cout << std::endl; }
					std::cout << "STATS " << mod << " : ICR ";
					for (size_t ch = 0; ch < pif.GetNumberChannels(); ch++) {
						std::cout.precision(2);
						std::cout << " " << pif.GetInputCountRate(mod, ch);
					}
					std::cout << std::endl << std::flush;
				}
				write_data(fifoData, dataWords);
				dataWords = 0;
				statsTime += usGetDTime();
				lastStatsTime = usGetTime(startTime);
			}
			
			// check whether it is time to dump histograms
			if(HISTO_INTERVAL != -1 && usGetTime(startTime) > lastHistoTime + HISTO_INTERVAL * 1e6){
				usGetDTime(); // start timer
				std::ofstream out("histo.dat", std::ios::trunc);
				std::ofstream deltaout("deltahisto.dat", std::ios::trunc);

				for (size_t mod=0; mod < N_CARDS; mod++) {
					for (size_t ch=0; ch < pif.GetNumberChannels(); ch++) {
						chanid_t id(mod, ch);
						PixieInterface::Histogram &histo = histoMap[id];

						// Copy the old histogram data to the delta histogram temporarily
						deltaHisto = histo;
						
						// Performance improvement possible using Pixie16EMbufferIO directly to fetch all channels
						histo.Read(pif, mod, ch);
						histo.Write(out);
						
						// Calculate the change using the temporarily stored previous histogram
						deltaHisto = histo - deltaHisto;
						deltaHisto.Write(deltaout);
					}
				}
				out.close();
				deltaout.close();

				histoTime += usGetDTime();
				lastHistoTime = usGetTime(startTime);
			}
						
			// Check whether we have any data
			usGetDTime(); // Start timer
			for (unsigned int timeout = 0; timeout < (STOP_RUN ? 1 : POLL_TRIES); timeout++){ // See if the modules have any data for us
				for (size_t mod = 0; mod < N_CARDS; mod++) {
					if(!runDone[mod]){ nWords[mod] = pif.CheckFIFOWords(mod); }
					else{ nWords[mod] = 0; }
				}
				maxWords = std::max_element(nWords.begin(), nWords.end());
				if(*maxWords > threshWords){ break; }
				usleep(POLL_PAUSE);
			}
			time(&pollClock);
			pollTime = usGetDTime();
			
			int maxmod = maxWords - nWords.begin();
			bool readData = (*maxWords > threshWords || STOP_RUN);
			if(FORCE_SPILL){
				if(!readData){ readData = true; }
				FORCE_SPILL = false;
			}
			if(readData){
				// if not timed out, we have data to read	
				// read the data, starting with the module with the most words			
				int mod = maxmod;			
				mod = maxmod = 0; //! tmp, read out in a fixed order

				usleep(READ_PAUSE);
				do{
					bool fullFIFO = (nWords[mod] == EXTERNAL_FIFO_LENGTH);
					if(nWords[mod] > 0){
						usGetDTime(); // Start read timer
						word_t &bufferLength = fifoData[dataWords];

						// fifoData[dataWords++] = nWords[mod] + 2
						fifoData[dataWords++] = nWords[mod] + partialEventWords[mod] + 2;
						fifoData[dataWords++] = mod;
						word_t beginData = dataWords;

						// Only add to fifo stream if we have enough words to complete event?
						if(partialEventWords[mod] > 0){
							memcpy(&fifoData[dataWords], partialEventData[mod], sizeof(word_t) * partialEventWords[mod]);
							dataWords += partialEventWords[mod];
							partialEventWords[mod] = 0;
						}

						if(!pif.ReadFIFOWords(&fifoData[dataWords], nWords[mod], mod)){
							std::cout << "Error reading FIFO, bailing out!" << std::endl;
							// Something is wrong
							//BailOut(SEND_ALARM, alarmArgument);
						} 
						else{
							word_t parseWords = beginData;
							word_t eventSize;

							waitWords[mod] = 0; // No longer waiting (hopefully)
							readTime += usGetDTime(); // And starts parse timer
						
							do{ // Unfortuantely, we have to parse the data to make sure, we grabbed complete events
								word_t slotRead = ((fifoData[parseWords] & 0xF0) >> 4);
								word_t chanRead = (fifoData[parseWords] & 0xF);
								word_t slotExpected = pif.GetSlotNumber(mod);
								bool virtualChannel = ((fifoData[parseWords] & 0x20000000) != 0);

								eventSize = ((fifoData[parseWords] & 0x1FFE0000) >> 17);
								if(!virtualChannel && statsHandler){ statsHandler->AddEvent(mod, chanRead, sizeof(word_t) * eventSize); }

								if(eventSize == 0 || slotRead != slotExpected){
									if( slotRead != slotExpected ){ std::cout << "Slot read (" << slotRead << ") not the same as" << " module expected (" << slotExpected << ")" << std::endl; }
									if(eventSize == 0){ std::cout << "ZERO EVENT SIZE" << std::endl; }
									std::cout << "First header words: " << std::hex << fifoData[parseWords] << " " << fifoData[parseWords + 1] << " " << fifoData[parseWords + 2];
									std::cout << " at position " << std::dec << parseWords << "\n	parse started at position " << beginData << " reading " << nWords[mod] << " words." << std::endl;
									//! how to proceed from here
									// BailOut(SEND_ALARM, alarmArgument);
									//--------- THIS IS A ROUGH HACK TO FIX THE CORRUPT DATA ISSUE
									//goto read_again;
								}
								parseWords += eventSize;				
							} while(parseWords < dataWords + nWords[mod]);		 
							parseTime += usGetDTime();

							if(parseWords > dataWords + nWords[mod]){
								waitCounter++;
								// If we have ended the run, we should have all the words
								if(STOP_RUN){
									std::cout << Display::ErrorStr("Words missing at end of run.") << std::endl;
									//BailOut(SEND_ALARM, alarmArgument);
								} 
								else{ // we have a deficit of words, now we must wait for the remainder
									if( fullFIFO ){ // the FIFO was full so the rest of the partial event is likely lost
										parseWords -= eventSize;
										// update the buffer length
										nWords[mod]	= parseWords;
										bufferLength = nWords[mod] + 2;
									} 
									else{
										waitWords[mod] = parseWords - (dataWords + nWords[mod]);
										unsigned int timeout = 0;

										usGetDTime(); // start wait timer

										if(!IS_QUIET){ std::cout << "Waiting for " << waitWords[mod] << " words in module " << mod << std::flush; }

										usleep(WAIT_PAUSE);
										word_t testWords;

										while (timeout++ < WAIT_TRIES){
											testWords = pif.CheckFIFOWords(mod);
											if(testWords >= std::max(waitWords[mod], 2U)){ break; }
											usleep(POLL_PAUSE);
										} 
										waitTime += usGetDTime();

										if(timeout >= WAIT_TRIES){
											if(!IS_QUIET){ std::cout << " --- TIMED OUT," << std::endl << Display::InfoStr("\t\tmoving partial event to next buffer") << std::endl; }
											partialBufferCounter++;
											partialEventWords[mod] = eventSize - waitWords[mod];
											memcpy(partialEventData[mod], &fifoData[dataWords + nWords[mod] - partialEventWords[mod]], sizeof(word_t) * partialEventWords[mod]);
											nWords[mod] = parseWords - beginData - eventSize;

											// Update the size of the buffer;
											bufferLength = nWords[mod] + 2;
										} 
										else{
											if(!IS_QUIET){ std::cout << std::endl; }
											usleep(READ_PAUSE);
											int testWords = pif.CheckFIFOWords(mod);
											if(!pif.ReadFIFOWords(&fifoData[dataWords + nWords[mod]], waitWords[mod], mod)){
												std::cout << "Error reading FIFO, bailing out!" << std::endl;

												//BailOut(SEND_ALARM, alarmArgument); // Something is wrong 
												KILL_ALL = true;
											} 
											else{
												nWords[mod] += waitWords[mod];
											
												// no longer waiting for words
												waitWords[mod] = 0;
											
												// and update the length of the buffer
												bufferLength = nWords[mod] + 2;
											} // check success of read
										} // if we DID NOT time out waiting for words
									} // if we DID NOT have a full FIFO
								} // if we ARE NOT on the final read at the end of a run
							} // if there are words remaining	
							else{ nonWaitCounter++; }
						} // check success of read
					} 
					else{ // If we had any words
						// Write an empty buffer if there is no data
						fifoData[dataWords++] = 2;
						fifoData[dataWords++] = mod;			
					}
					if(nWords[mod] > EXTERNAL_FIFO_LENGTH * 9 / 10){
						std::cout << "Abnormally full FIFO with " << nWords[mod] << " words in module " << mod << std::endl;
						if(fullFIFO){
							pif.EndRun();
							STOP_RUN = true;
						}
					}
					if(!IS_QUIET){
						if(fullFIFO){ std::cout << "Read " << Display::WarningStr("full FIFO") << " in"; }
						else{ std::cout << "Read " << nWords[mod] << " words from"; }
					
						std::cout << " module " << mod << " to buffer position " << dataWords;
						if(partialEventWords[mod] > 0){ std::cout << ", " << partialEventWords[mod] << " words reserved."; }
						std::cout << std::endl;
					}
					dataWords += nWords[mod];
	
					// Read the remainder of the modules in a modulo ring
					mod = (mod + 1) % N_CARDS;
				} while(mod != maxmod);
			} // If we have data to read 

			// If we don't have enough words, poll socket and modules once more
			if(!readData){ continue; }
			
			if(INSERT_WALL_CLOCK){
				// Add the "wall time" in artificially
				size_t timeWordsNeeded = sizeof(time_t) / sizeof(word_t);
				if((sizeof(time_t) % sizeof(word_t)) != 0){ timeWordsNeeded++; }
				fifoData[dataWords++] = 2 + timeWordsNeeded;
				fifoData[dataWords++] = CLOCK_VSN;
				memcpy(&fifoData[dataWords], &pollClock, sizeof(time_t));
				if(!IS_QUIET){ std::cout << "Read " << timeWordsNeeded << " words for time to buffer position " << dataWords << std::endl; }
				dataWords += timeWordsNeeded;
			}
			
			spillTime = usGetTime(startTime);
			durSpill = spillTime - lastSpillTime;
			lastSpillTime = spillTime;

			usGetDTime(); // start send timer
			
			int nBufs = write_data(fifoData, dataWords);
			sendTime = usGetDTime();

			if(statsHandler){ statsHandler->AddTime(durSpill * 1e-6); }
			
			if (!IS_QUIET) {
				std::cout << nBufs << " BUFFERS with " << dataWords << " WORDS, " << std::endl;
				std::cout.setf(std::ios::scientific, std::ios::floatfield);
				std::cout.precision(1);
				std::cout << "    SPILL " << durSpill << " us " << " POLL  " << pollTime << " us " << " PARSE " << parseTime << " us" << std::endl;
				std::cout << "    WAIT  " << waitTime << " us " << " READ  " << readTime << " us " << " SEND  " << sendTime << " us" << std::endl;	 
				
				// Add some blank spaces so STATS or HISTO line up
				std::cout << "   ";
				if(STATS_INTERVAL != -1){ std::cout << " STATS " << statsTime << " us "; }
				if(HISTO_INTERVAL != -1){ std::cout << " HISTO " << histoTime << " us "; }
				if(STATS_INTERVAL != -1 || HISTO_INTERVAL != -1){ std::cout << std::endl; }
				std::cout << std::endl;
			} 
			else{
				std::cout.setf(std::ios::scientific, std::ios::floatfield);
				std::cout.precision(1);

				if(!SHOW_MODULE_RATES){ std::cout << nBufs << " bufs : " << "SEND " << sendTime << " / SPILL " << durSpill << "     \r"; } 
				else if(statsHandler){      
					for(size_t i=0; i < N_CARDS; i++){
						std::cout << "M" << i << ", " << statsHandler->GetEventRate(i) / 1000. << " kHz";
						std::cout << " (" << statsHandler->GetDataRate(i) / 1000000. << " MB/s)";
					}  
					std::cout << "    \r";
				}
			}
			
			// Reset the number of words of fifo data
			dataWords = 0;
			histoTime = statsTime = 0;
			if(STOP_RUN){ 
				time(&RAW_TIME);
				TIME_INFO = localtime(&RAW_TIME);
				std::cout << SYS_MESSAGE_HEAD << "Stopping run at " << asctime(TIME_INFO);
				pif.EndRun();
				
				STOP_RUN = false;
				POLL_RUNNING = false;
				usleep(END_RUN_PAUSE);	
				
				// Update whether the run has ended with the data read out
				for(size_t mod = 0; mod < N_CARDS; mod++){
					if(!pif.CheckRunStatus(mod)){
						runDone[mod] = true;
						std::cout << "Run ended in module " << mod << std::endl;
					}
					if(pif.CheckRunStatus(mod)){
						std::cout << "Run not properly finished in module " << mod << std::endl;
					}
				}			
			}
		}
		else{ 	
			if(START_RUN){
				if(!OUTPUT_FILE.IsOpen()){ 
					if(!OUTPUT_FILE.OpenNewFile(OUTPUT_TITLE, OUTPUT_RUN_NUM, CURRENT_FILENAME, OUTPUT_DIRECTORY)){
						std::cout << SYS_MESSAGE_HEAD << "Failed to open output file! Check that the path is correct.\n";
						START_RUN = false;
						continue;
					}
					std::cout << SYS_MESSAGE_HEAD << "Opening output file '" << CURRENT_FILENAME << "'.\n";
				}
				if(ZERO_CLOCKS){ synch_mods(pif); }
				for(size_t mod=0; mod < N_CARDS; mod++){ runDone[mod] = false; }
				lastHistoTime = lastStatsTime = lastSpillTime = usGetTime(startTime);
				
				time(&RAW_TIME);
				TIME_INFO = localtime(&RAW_TIME);
				std::cout << SYS_MESSAGE_HEAD << "Starting run at " << asctime(TIME_INFO);				
				if(pif.StartListModeRun(LIST_MODE_RUN, NEW_RUN)){
					POLL_RUNNING = true;
					nonWaitCounter = 0;
				}
				else{ std::cout << SYS_MESSAGE_HEAD << "Failed to start list mode run. Try rebooting PIXIE\n"; }
				START_RUN = false;
			}
			else if(STOP_RUN){
				std::cout << SYS_MESSAGE_HEAD << "Not running!\n";
				STOP_RUN = false;
			}
			sleep(1); 
		}
	}

	if(waitCounter + nonWaitCounter != 0){ 
		std::cout << "Waiting for " << waitCounter * 100 / (waitCounter + nonWaitCounter) << "% of the spills." << std::endl;
		std::cout << "  " << partialBufferCounter << " partial buffers" << std::endl;
	}

	RUN_CTRL_EXIT = true;
}

/* Print help dialogue for command line options. */
void help(){
	std::cout << "\n SYNTAX: ./poll2 [options]\n";
	std::cout << "  -a, --alarm=[e-mail] Call the alarm script with a given e-mail (or no argument)\n"; 
	std::cout << "  -f, --fast           Fast boot (false by default)\n";
	std::cout << "  -h, --hist <num>     Dump histogram data every num seconds\n";
	std::cout << "  -q, --quiet          Run quietly (false by default)\n";
	std::cout << "  -n, --no-wall-clock  Do not insert the wall clock in the data stream\n";
	std::cout << "  -r, --rates          Display module rates in quiet mode (false by defualt)\n";
	std::cout << "  -s, --stats <num>    Output statistics data every num seconds\n";
	std::cout << "  -t, --thresh <num>   Sets FIFO read threshold to num% full (50% by default)\n";
	std::cout << "  -z, --zero           Zero clocks on each START_ACQ (false by default)\n";
	std::cout << "  -d, --debug          Set debug mode to true (false by default)\n";
	std::cout << "  -m, --memory-share   Do not write data to shared memory (SHM) (true by default)\n\n";
}

/* Parse all command line entries and find valid options. */
bool get_opt(int argc_, char **argv_, CLoption *options, unsigned int num_valid_opt_){
	unsigned int index = 1;
	unsigned int previous_opt;
	bool need_an_argument = false;
	bool may_have_argument = false;
	bool is_valid_argument = false;
	while(index < argc_){
		if(argv_[index][0] == '-'){
			if(need_an_argument){
				std::cout << " Error: --" << options[previous_opt].alias << " [-" << options[previous_opt].opt << "] requires an argument\n";
				help();
				return false;
			}

			is_valid_argument = false;
			if(argv_[index][1] == '-'){ // Word options
				std::string word_arg = csubstr(argv_[index], 2);
				for(unsigned int i = 0; i < num_valid_opt_; i++){
					if(word_arg == options[i].alias && word_arg != "NULL"){
						options[i].is_active = true; 
						previous_opt = i;
						if(options[i].require_arg){ need_an_argument = true; }
						else{ need_an_argument = false; }
						if(options[i].optional_arg){ may_have_argument = true; }
						else{ may_have_argument = false; }
						is_valid_argument = true;
					}
				}
				if(!is_valid_argument){
					std::cout << " Error: encountered unknown option --" << word_arg << std::endl;
					help();
					return false;
				}
			}
			else{ // Character options
				unsigned int index2 = 1;
				while(argv_[index][index2] != '\0'){
					for(unsigned int i = 0; i < num_valid_opt_; i++){
						if(argv_[index][index2] == options[i].opt && argv_[index][index2] != 0x0){ 
							options[i].is_active = true; 
							previous_opt = i;
							if(options[i].require_arg){ need_an_argument = true; }
							else{ need_an_argument = false; }
							if(options[i].optional_arg){ may_have_argument = true; }
							else{ may_have_argument = false; }
							is_valid_argument = true;
						}
					}
					if(!is_valid_argument){
						std::cout << " Error: encountered unknown option -" << argv_[index][index2] << std::endl;
						help();
						return false;
					}
					index2++;
				}
			}
		}
		else{ // An option argument
			if(need_an_argument || may_have_argument){
				options[previous_opt].value = csubstr(argv_[index]);
				need_an_argument = false;
				may_have_argument = false;
			}
			else{
				std::cout << " Error: --" << options[previous_opt].alias << " [-" << options[previous_opt].opt << "] takes no argument\n";
				help();
				return false;			
			}
		}
		
		// Check for the case where the end option requires an argument, but did not receive it
		if(index == argc_-1 && need_an_argument){
			std::cout << " Error: --" << options[previous_opt].alias << " [-" << options[previous_opt].opt << "] requires an argument\n";
			help();
			return false;	
		}
		
		index++;
	}
	return true;
}

/* Return the length of a character string. */
unsigned int cstrlen(char *str_){
	unsigned int output = 0;
	while(true){
		if(str_[output] == '\0' || str_[output] == ' '){ break; }
		output++;
	}
	return output;
}

/* Extract a string from a character array. */
std::string csubstr(char *str_, unsigned int start_index_){
	std::string output = "";
	unsigned int index = start_index_;
	while(str_[index] != '\0' && str_[index] != ' '){
		output += str_[index];
		index++;
	}
	return output;
}

/* Pad a string with '.' to a specified length. */
std::string pad_string(const std::string &input_, unsigned int length_){
	std::string output = input_;
	for(unsigned int i = input_.size(); i <= length_; i++){
		output += '.';
	}
	return output;
}

std::string yesno(bool value_){
	if(value_){ return "Yes"; }
	return "No";
}
