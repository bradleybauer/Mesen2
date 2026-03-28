#pragma once
#include "pch.h"
#include <thread>
#include "Utilities/AutoResetEvent.h"
#include "Utilities/SimpleLock.h"
#include "Core/Shared/SettingTypes.h"
#include "Core/Shared/ControlDeviceState.h"

class Emulator;

struct RecordingOptions
{
	uint32_t SaveStateIntervalFrames = 60; // 1 Hz at 60fps, 0 = disabled
};

class DataCollector
{
private:
	// NPY format: 128-byte reserved header allows up to ~10-digit frame counts
	static constexpr uint32_t NpyReservedHeaderSize = 128;

	std::thread _writerThread;

	string _basePath;
	std::ofstream _npyFrames;
	std::ofstream _npyRam;
	std::ofstream _npyWram;
	std::ofstream _npyInput;
	SimpleLock _lock;
	AutoResetEvent _waitFrame;

	atomic<bool> _stopFlag;
	atomic<bool> _framePending;
	bool _recording = false;

	// Per-recording constants (set in StartRecording)
	uint32_t _ramSize = 0;
	uint32_t _wramSize = 0;
	uint16_t _inputBytesPerFrame = 0;
	uint32_t _recordSize = 0; // 4 + _ramSize + _wramSize + _inputBytesPerFrame
	uint32_t _startFrameNumber = 0;

	// Double-buffer: emulation thread writes to _captureBuffer, writer thread reads from _writeBuffer
	vector<uint8_t> _captureBuffer;
	vector<uint8_t> _writeBuffer;

	// Periodic save states
	RecordingOptions _options;
	uint32_t _framesSinceLastSaveState = 0;
	uint32_t _recordedFrameCount = 0;

	void WriteNpyHeader(std::ofstream& file, const char* descr, int ndim, const uint32_t* shape);
	void FinalizeNpyHeaders();
	void WriterLoop();
	void SavePeriodicState(Emulator* emu, uint32_t frameNumber);

public:
	DataCollector();
	~DataCollector();

	bool StartRecording(string basePath, Emulator* emu, RecordingOptions options);
	void CaptureFrame(Emulator* emu);
	void StopRecording();
	bool IsRecording();
};
