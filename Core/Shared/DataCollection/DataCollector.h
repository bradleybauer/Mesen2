#pragma once
#include "pch.h"
#include <thread>
#include "Utilities/AutoResetEvent.h"
#include "Utilities/SimpleLock.h"
#include "Core/Shared/SettingTypes.h"
#include "Core/Shared/ControlDeviceState.h"

class Emulator;

struct ResearchRecordingOptions
{
	uint32_t SaveStateIntervalFrames = 1800; // ~30 seconds at 60fps, 0 = disabled
};

class DataCollector
{
private:
	static constexpr uint32_t MdatHeaderSize = 32;
	static constexpr char MdatMagic[4] = { 'M', 'S', 'D', 'C' };
	static constexpr uint16_t MdatVersion = 1;

	std::thread _writerThread;

	string _basePath;
	std::ofstream _file;
	SimpleLock _lock;
	AutoResetEvent _waitFrame;

	atomic<bool> _stopFlag;
	atomic<bool> _framePending;
	bool _recording = false;

	// Per-recording constants (set in StartRecording)
	uint32_t _ramSize = 0;
	uint32_t _wramSize = 0;
	uint16_t _inputBytesPerFrame = 0;
	uint16_t _numControllers = 0;
	uint32_t _recordSize = 0; // 4 + _ramSize + _wramSize + _inputBytesPerFrame
	uint32_t _startFrameNumber = 0;

	// Double-buffer: emulation thread writes to _captureBuffer, writer thread reads from _writeBuffer
	vector<uint8_t> _captureBuffer;
	vector<uint8_t> _writeBuffer;

	// Periodic save states
	ResearchRecordingOptions _options;
	uint32_t _framesSinceLastSaveState = 0;
	uint32_t _recordedFrameCount = 0;

	void WriteHeader(Emulator* emu);
	void WriterLoop();
	void SavePeriodicState(Emulator* emu, uint32_t frameNumber);

public:
	DataCollector();
	~DataCollector();

	bool StartRecording(string basePath, Emulator* emu, ResearchRecordingOptions options);
	void CaptureFrame(Emulator* emu);
	void StopRecording();
	bool IsRecording();
};
