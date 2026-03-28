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

	// Fixed 1 byte for port 1 input
	_inputBytesPerFrame = 1;

	// Calculate fixed record size: frame_number(4) + ram + wram + input(1)
	_recordSize = 4 + _ramSize + _wramSize + _inputBytesPerFrame;

	// Open .npy files with placeholder headers (shape[0] = 0, fixed at stop)
	_npyFrames.open(basePath + ".frames.npy", std::ios::out | std::ios::binary);
	_npyRam.open(basePath + ".ram.npy", std::ios::out | std::ios::binary);
	if(_wramSize > 0) {
		_npyWram.open(basePath + ".wram.npy", std::ios::out | std::ios::binary);
	}
	if(_inputBytesPerFrame > 0) {
		_npyInput.open(basePath + ".input.npy", std::ios::out | std::ios::binary);
	}

	if(!_npyFrames || !_npyRam) {
		MessageManager::DisplayMessage("ResearchRecording", "Could not open npy files: " + basePath);
		return false;
	}

	// Write placeholder npy headers (N=0, will be rewritten at stop)
	uint32_t zeroShape1[] = { 0 };
	uint32_t zeroShape2[] = { 0, _ramSize };
	WriteNpyHeader(_npyFrames, "<u4", 1, zeroShape1);
	WriteNpyHeader(_npyRam, "|u1", 2, zeroShape2);
	if(_wramSize > 0) {
		uint32_t wramShape[] = { 0, _wramSize };
		WriteNpyHeader(_npyWram, "|u1", 2, wramShape);
	}
	if(_inputBytesPerFrame > 0) {
		WriteNpyHeader(_npyInput, "|u1", 1, zeroShape1);
	}

	_startFrameNumber = emu->GetFrameCount();
	_framesSinceLastSaveState = 0;
	_recordedFrameCount = 0;

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

void DataCollector::WriteNpyHeader(std::ofstream& file, const char* descr, int ndim, const uint32_t* shape)
{
	// NPY v1.0 format: 6-byte magic + 2-byte version + 2-byte header_len + header_data
	// Total = NpyReservedHeaderSize (128 bytes, 64-byte aligned)
	uint8_t header[NpyReservedHeaderSize] = {};

	// Magic: \x93NUMPY
	header[0] = 0x93;
	header[1] = 'N'; header[2] = 'U'; header[3] = 'M'; header[4] = 'P'; header[5] = 'Y';

	// Version 1.0
	header[6] = 1; header[7] = 0;

	// Header data length = total - 10 (magic + version + header_len fields)
	uint16_t headerLen = NpyReservedHeaderSize - 10;
	memcpy(header + 8, &headerLen, 2);

	// Build Python dict literal
	char dictBuf[118];
	memset(dictBuf, ' ', 118);

	int pos;
	if(ndim == 1) {
		pos = snprintf(dictBuf, 118,
			"{'descr': '%s', 'fortran_order': False, 'shape': (%u,), }", descr, shape[0]);
	} else {
		pos = snprintf(dictBuf, 118,
			"{'descr': '%s', 'fortran_order': False, 'shape': (%u, %u), }", descr, shape[0], shape[1]);
	}
	// snprintf null-terminates; replace null with space, end with newline
	if(pos > 0 && pos < 117) dictBuf[pos] = ' ';
	dictBuf[117] = '\n';

	memcpy(header + 10, dictBuf, 118);
	file.write(reinterpret_cast<char*>(header), NpyReservedHeaderSize);
	file.flush();
}

void DataCollector::FinalizeNpyHeaders()
{
	uint32_t N = _recordedFrameCount;

	if(_npyFrames.is_open()) {
		_npyFrames.seekp(0);
		uint32_t shape[] = { N };
		WriteNpyHeader(_npyFrames, "<u4", 1, shape);
	}

	if(_npyRam.is_open()) {
		_npyRam.seekp(0);
		uint32_t shape[] = { N, _ramSize };
		WriteNpyHeader(_npyRam, "|u1", 2, shape);
	}

	if(_npyWram.is_open()) {
		_npyWram.seekp(0);
		uint32_t shape[] = { N, _wramSize };
		WriteNpyHeader(_npyWram, "|u1", 2, shape);
	}

	if(_npyInput.is_open()) {
		_npyInput.seekp(0);
		uint32_t shape[] = { N };
		WriteNpyHeader(_npyInput, "|u1", 1, shape);
	}
}

void DataCollector::WriterLoop()
{
	while(!_stopFlag) {
		_waitFrame.Wait();
		if(_stopFlag) {
			break;
		}

		auto lock = _lock.AcquireSafe();
		uint32_t offset = 0;

		// Frame number (4 bytes, uint32 LE)
		_npyFrames.write(reinterpret_cast<char*>(_writeBuffer.data() + offset), 4);
		offset += 4;

		// RAM
		_npyRam.write(reinterpret_cast<char*>(_writeBuffer.data() + offset), _ramSize);
		offset += _ramSize;

		// WRAM
		if(_wramSize > 0) {
			_npyWram.write(reinterpret_cast<char*>(_writeBuffer.data() + offset), _wramSize);
			offset += _wramSize;
		}

		// Input
		if(_inputBytesPerFrame > 0) {
			_npyInput.write(reinterpret_cast<char*>(_writeBuffer.data() + offset), _inputBytesPerFrame);
		}

		_framePending = false;
	}

	// Flush remaining data
	if(_npyFrames.is_open()) _npyFrames.flush();
	if(_npyRam.is_open()) _npyRam.flush();
	if(_npyWram.is_open()) _npyWram.flush();
	if(_npyInput.is_open()) _npyInput.flush();
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

	// Pack port 1 input (1 byte)
	{
		shared_ptr<IConsole> console = emu->GetConsole();
		if(console) {
			vector<ControllerData> portStates = console->GetControlManager()->GetPortStates();
			_captureBuffer[offset] = 0;
			for(auto& ctrl : portStates) {
				if(ctrl.Port == 0 && !ctrl.State.State.empty()) {
					_captureBuffer[offset] = ctrl.State.State[0];
					break;
				}
			}
		} else {
			_captureBuffer[offset] = 0;
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

	// Rewrite npy headers with actual frame count
	FinalizeNpyHeaders();

	// Close files
	if(_npyFrames.is_open()) _npyFrames.close();
	if(_npyRam.is_open()) _npyRam.close();
	if(_npyWram.is_open()) _npyWram.close();
	if(_npyInput.is_open()) _npyInput.close();

	MessageManager::DisplayMessage("ResearchRecording", "Stopped. Frames: " + std::to_string(_recordedFrameCount));
}

bool DataCollector::IsRecording()
{
	return _recording;
}
