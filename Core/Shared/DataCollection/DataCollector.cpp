#include "pch.h"
#include "DataCollector.h"
#include "Core/Shared/Emulator.h"
#include "Core/Shared/Interfaces/IConsole.h"
#include "Core/Shared/BaseControlManager.h"
#include "Core/Shared/SaveStateManager.h"
#include "Core/Shared/MessageManager.h"
#include "Utilities/FolderUtilities.h"

DataCollector::DataCollector()
{
	_stopFlag = false;
	_framePending = false;
	_recording = false;
}

DataCollector::~DataCollector()
{
	if(_recording) {
		StopRecording();
	}
}

bool DataCollector::StartRecording(string basePath, Emulator* emu, ResearchRecordingOptions options)
{
	if(_recording) {
		return false;
	}

	_basePath = basePath;
	_options = options;

	// Query memory sizes from the emulator's registered memory regions
	ConsoleMemoryInfo ramInfo = emu->GetMemory(MemoryType::NesInternalRam);
	ConsoleMemoryInfo wramInfo = emu->GetMemory(MemoryType::NesWorkRam);

	_ramSize = ramInfo.Size;
	_wramSize = wramInfo.Memory ? wramInfo.Size : 0;

	// Query input layout from current controller state
	shared_ptr<IConsole> console = emu->GetConsole();
	if(!console) {
		return false;
	}

	vector<ControllerData> portStates = console->GetControlManager()->GetPortStates();
	_numControllers = (uint16_t)portStates.size();
	_inputBytesPerFrame = 0;
	for(auto& controller : portStates) {
		_inputBytesPerFrame += (uint16_t)controller.State.State.size();
	}

	// Calculate fixed record size: frame_number(4) + ram + wram + input
	_recordSize = 4 + _ramSize + _wramSize + _inputBytesPerFrame;

	// Open .mdat file
	string mdatPath = basePath + ".mdat";
	_file.open(mdatPath, std::ios::out | std::ios::binary);
	if(!_file) {
		MessageManager::DisplayMessage("ResearchRecording", "Could not open file: " + mdatPath);
		return false;
	}

	_startFrameNumber = emu->GetFrameCount();
	_framesSinceLastSaveState = 0;
	_recordedFrameCount = 0;

	// Write header
	WriteHeader(emu);

	// Save initial save state
	string mssPath = basePath + ".mss";
	emu->GetSaveStateManager()->SaveState(mssPath, false);

	// Create periodic save state directory if interval is set
	if(_options.SaveStateIntervalFrames > 0) {
		string statesDir = basePath + "_states";
		FolderUtilities::CreateFolder(statesDir);
	}

	// Allocate buffers
	_captureBuffer.resize(_recordSize);
	_writeBuffer.resize(_recordSize);

	// Start writer thread
	_stopFlag = false;
	_framePending = false;
	_writerThread = std::thread(&DataCollector::WriterLoop, this);

	_recording = true;

	MessageManager::DisplayMessage("ResearchRecording", "Started: " + basePath);
	return true;
}

void DataCollector::WriteHeader(Emulator* emu)
{
	uint8_t header[MdatHeaderSize] = {};

	// [0-3] magic "MSDC"
	memcpy(header + 0, MdatMagic, 4);

	// [4-5] version
	uint16_t version = MdatVersion;
	memcpy(header + 4, &version, 2);

	// [6-7] console_type
	uint16_t consoleType = (uint16_t)emu->GetConsoleType();
	memcpy(header + 6, &consoleType, 2);

	// [8-11] ram_size
	memcpy(header + 8, &_ramSize, 4);

	// [12-15] wram_size
	memcpy(header + 12, &_wramSize, 4);

	// [16-17] input_bytes_per_frame
	memcpy(header + 16, &_inputBytesPerFrame, 2);

	// [18-19] num_controllers
	memcpy(header + 18, &_numControllers, 2);

	// [20-23] start_frame_number
	memcpy(header + 20, &_startFrameNumber, 4);

	// [24-27] fps_times_1000
	uint32_t fpsX1000 = (uint32_t)(emu->GetFps() * 1000.0);
	memcpy(header + 24, &fpsX1000, 4);

	// [28-31] reserved
	// already zero from initialization

	_file.write(reinterpret_cast<char*>(header), MdatHeaderSize);
	_file.flush();
}

void DataCollector::WriterLoop()
{
	while(!_stopFlag) {
		_waitFrame.Wait();
		if(_stopFlag) {
			break;
		}

		auto lock = _lock.AcquireSafe();
		_file.write(reinterpret_cast<char*>(_writeBuffer.data()), _recordSize);
		_framePending = false;
	}

	// Flush remaining data
	if(_file.is_open()) {
		_file.flush();
	}
}

void DataCollector::CaptureFrame(Emulator* emu)
{
	if(!_recording) {
		return;
	}

	uint32_t frameNumber = emu->GetFrameCount();
	uint32_t offset = 0;

	// Pack frame number
	memcpy(_captureBuffer.data() + offset, &frameNumber, 4);
	offset += 4;

	// Pack RAM
	ConsoleMemoryInfo ramInfo = emu->GetMemory(MemoryType::NesInternalRam);
	if(ramInfo.Memory && _ramSize > 0) {
		memcpy(_captureBuffer.data() + offset, ramInfo.Memory, _ramSize);
	}
	offset += _ramSize;

	// Pack WRAM
	if(_wramSize > 0) {
		ConsoleMemoryInfo wramInfo = emu->GetMemory(MemoryType::NesWorkRam);
		if(wramInfo.Memory) {
			memcpy(_captureBuffer.data() + offset, wramInfo.Memory, _wramSize);
		} else {
			memset(_captureBuffer.data() + offset, 0, _wramSize);
		}
	}
	offset += _wramSize;

	// Pack input state
	if(_inputBytesPerFrame > 0) {
		shared_ptr<IConsole> console = emu->GetConsole();
		if(console) {
			vector<ControllerData> portStates = console->GetControlManager()->GetPortStates();
			uint32_t inputOffset = offset;
			for(auto& controller : portStates) {
				uint16_t stateSize = (uint16_t)controller.State.State.size();
				if(inputOffset + stateSize <= _recordSize) {
					memcpy(_captureBuffer.data() + inputOffset, controller.State.State.data(), stateSize);
					inputOffset += stateSize;
				}
			}
			// Zero any remaining input bytes if controllers changed
			if(inputOffset < offset + _inputBytesPerFrame) {
				memset(_captureBuffer.data() + inputOffset, 0, offset + _inputBytesPerFrame - inputOffset);
			}
		} else {
			memset(_captureBuffer.data() + offset, 0, _inputBytesPerFrame);
		}
	}

	// Wait for previous frame to finish writing
	while(_framePending) {
		std::this_thread::sleep_for(std::chrono::duration<int, std::milli>(1));
	}

	// Swap buffer to writer thread
	{
		auto lock = _lock.AcquireSafe();
		_framePending = true;
		std::swap(_captureBuffer, _writeBuffer);
		_waitFrame.Signal();
	}

	_recordedFrameCount++;

	// Periodic save state
	if(_options.SaveStateIntervalFrames > 0) {
		_framesSinceLastSaveState++;
		if(_framesSinceLastSaveState >= _options.SaveStateIntervalFrames) {
			SavePeriodicState(emu, frameNumber);
			_framesSinceLastSaveState = 0;
		}
	}
}

void DataCollector::SavePeriodicState(Emulator* emu, uint32_t frameNumber)
{
	string statesDir = _basePath + "_states";
	char filename[64];
	snprintf(filename, sizeof(filename), "/frame_%08u.mss", frameNumber);
	string filepath = statesDir + filename;
	emu->GetSaveStateManager()->SaveState(filepath, false);
}

void DataCollector::StopRecording()
{
	if(!_recording) {
		return;
	}

	_recording = false;

	// Signal writer thread to stop
	_stopFlag = true;
	_waitFrame.Signal();
	_writerThread.join();

	// Close file
	if(_file.is_open()) {
		_file.close();
	}

	MessageManager::DisplayMessage("ResearchRecording", "Stopped. Frames: " + std::to_string(_recordedFrameCount));
}

bool DataCollector::IsRecording()
{
	return _recording;
}
