#include "pch.h"
#include "DataCollector.h"
#include "Core/Shared/Emulator.h"
#include "Core/Shared/DebuggerRequest.h"
#include "Core/Shared/Interfaces/IConsole.h"
#include "Core/Shared/BaseControlManager.h"
#include "Core/Shared/SaveStateManager.h"
#include "Core/Shared/MessageManager.h"
#include "Core/Debugger/Debugger.h"
#include "Core/Debugger/MemoryDumper.h"
#include "Core/Debugger/PpuTools.h"
#include "Core/SNES/Debugger/SnesPpuTools.h"
#include "Core/SNES/SnesPpuTypes.h"
#include "Core/Gameboy/GbTypes.h"
#include "Core/NES/Debugger/NesPpuTools.h"
#include "Core/NES/NesTypes.h"
#include "Core/PCE/PceTypes.h"
#include "Core/SMS/SmsTypes.h"
#include "Core/GBA/GbaTypes.h"
#include "Core/WS/WsTypes.h"
#include "Utilities/FolderUtilities.h"

namespace
{
	static constexpr uint32_t SpritePreviewSize = 128 * 128;

	MemoryType GetVramMemoryType(CpuType cpuType, bool getExtendedRam = false)
	{
		switch(cpuType) {
			case CpuType::Snes: return MemoryType::SnesVideoRam;
			case CpuType::Gameboy: return MemoryType::GbVideoRam;
			case CpuType::Nes: return MemoryType::NesPpuMemory;
			case CpuType::Pce: return getExtendedRam ? MemoryType::PceVideoRamVdc2 : MemoryType::PceVideoRam;
			case CpuType::Sms: return MemoryType::SmsVideoRam;
			case CpuType::Gba: return MemoryType::GbaVideoRam;
			case CpuType::Ws: return MemoryType::WsWorkRam;
			default: return MemoryType::None;
		}
	}

	MemoryType GetSpriteRamMemoryType(CpuType cpuType, bool getExtendedRam = false)
	{
		switch(cpuType) {
			case CpuType::Snes: return MemoryType::SnesSpriteRam;
			case CpuType::Gameboy: return MemoryType::GbSpriteRam;
			case CpuType::Nes: return MemoryType::NesSpriteRam;
			case CpuType::Pce: return getExtendedRam ? MemoryType::PceSpriteRamVdc2 : MemoryType::PceSpriteRam;
			case CpuType::Gba: return MemoryType::GbaSpriteRam;
			default: return MemoryType::None;
		}
	}

	uint32_t GetPpuStateSize(CpuType cpuType)
	{
		switch(cpuType) {
			case CpuType::Snes:
			case CpuType::Spc:
			case CpuType::NecDsp:
			case CpuType::Sa1:
			case CpuType::Gsu:
			case CpuType::Cx4:
			case CpuType::St018:
				return sizeof(SnesPpuState);

			case CpuType::Gameboy: return sizeof(GbPpuState);
			case CpuType::Nes: return sizeof(NesPpuState);
			case CpuType::Pce: return sizeof(PceVideoState);
			case CpuType::Sms: return sizeof(SmsVdpState);
			case CpuType::Gba: return sizeof(GbaPpuState);
			case CpuType::Ws: return sizeof(WsPpuState);
		}

		return sizeof(BaseState);
	}

	uint32_t GetPpuToolsStateSize(CpuType cpuType)
	{
		switch(cpuType) {
			case CpuType::Snes:
			case CpuType::Spc:
			case CpuType::NecDsp:
			case CpuType::Sa1:
			case CpuType::Gsu:
			case CpuType::Cx4:
			case CpuType::St018:
				return sizeof(SnesPpuToolsState);

			case CpuType::Nes: return sizeof(NesPpuToolsState);
			default: return sizeof(BaseState);
		}
	}

	void ResizeStateBuffer(vector<uint64_t>& buffer, uint32_t byteSize)
	{
		buffer.resize((byteSize + sizeof(uint64_t) - 1) / sizeof(uint64_t));
	}

	BaseState& AsBaseState(vector<uint64_t>& buffer)
	{
		return *reinterpret_cast<BaseState*>(buffer.data());
	}
}

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

bool DataCollector::StartRecording(string basePath, Emulator* emu, RecordingOptions options)
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
	_recordSpriteMask = InitializeSpriteMaskRecording(emu);

	// Calculate fixed record size: frame_number(4) + ram + wram + input(1) + optional sprite mask
	_recordSize = 4 + _ramSize + _wramSize + _inputBytesPerFrame + (_recordSpriteMask ? _spriteMaskSize : 0);

	// Open .npy files with placeholder headers (shape[0] = 0, fixed at stop)
	_npyFrames.open(basePath + ".frames.npy", std::ios::out | std::ios::binary);
	_npyRam.open(basePath + ".ram.npy", std::ios::out | std::ios::binary);
	if(_wramSize > 0) {
		_npyWram.open(basePath + ".wram.npy", std::ios::out | std::ios::binary);
	}
	if(_inputBytesPerFrame > 0) {
		_npyInput.open(basePath + ".input.npy", std::ios::out | std::ios::binary);
	}
	if(_recordSpriteMask) {
		_npySpriteMask.open(basePath + ".sprite_mask.npy", std::ios::out | std::ios::binary);
	}

	if(!_npyFrames || !_npyRam || (_recordSpriteMask && !_npySpriteMask)) {
		MessageManager::DisplayMessage("DataRecording", "Could not open npy files: " + basePath);
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
	if(_recordSpriteMask) {
		uint32_t spriteMaskShape[] = { 0, _spriteMaskHeight, _spriteMaskWidth };
		WriteNpyHeader(_npySpriteMask, "|b1", 3, spriteMaskShape);
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

	MessageManager::DisplayMessage("DataRecording", "Started: " + basePath);
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
	} else if(ndim == 2) {
		pos = snprintf(dictBuf, 118,
			"{'descr': '%s', 'fortran_order': False, 'shape': (%u, %u), }", descr, shape[0], shape[1]);
	} else {
		pos = snprintf(dictBuf, 118,
			"{'descr': '%s', 'fortran_order': False, 'shape': (%u, %u, %u), }", descr, shape[0], shape[1], shape[2]);
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

	if(_npySpriteMask.is_open()) {
		_npySpriteMask.seekp(0);
		uint32_t shape[] = { N, _spriteMaskHeight, _spriteMaskWidth };
		WriteNpyHeader(_npySpriteMask, "|b1", 3, shape);
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
			offset += _inputBytesPerFrame;
		}

		// PPU sprite mask
		if(_recordSpriteMask) {
			_npySpriteMask.write(reinterpret_cast<char*>(_writeBuffer.data() + offset), _spriteMaskSize);
		}

		_framePending = false;
	}

	// Flush remaining data
	if(_npyFrames.is_open()) _npyFrames.flush();
	if(_npyRam.is_open()) _npyRam.flush();
	if(_npyWram.is_open()) _npyWram.flush();
	if(_npyInput.is_open()) _npyInput.flush();
	if(_npySpriteMask.is_open()) _npySpriteMask.flush();
}

bool DataCollector::InitializeSpriteMaskRecording(Emulator* emu)
{
	_recordSpriteMask = false;
	_spriteMaskWidth = 0;
	_spriteMaskHeight = 0;
	_spriteMaskSize = 0;

	DebuggerRequest dbgRequest = emu->GetDebugger(true);
	Debugger* debugger = dbgRequest.GetDebugger();
	if(!debugger) {
		return false;
	}

	CpuType cpuType = debugger->GetMainCpuType();
	PpuTools* ppuTools = debugger->GetPpuTools(cpuType);
	if(!ppuTools) {
		return false;
	}

	ResizeStateBuffer(_ppuStateBuffer, GetPpuStateSize(cpuType));
	ResizeStateBuffer(_ppuToolsStateBuffer, GetPpuToolsStateSize(cpuType));
	debugger->GetPpuState(AsBaseState(_ppuStateBuffer), cpuType);
	ppuTools->GetPpuToolsState(AsBaseState(_ppuToolsStateBuffer));

	GetSpritePreviewOptions options = {};
	options.Background = SpriteBackground::Transparent;
	DebugSpritePreviewInfo previewInfo = ppuTools->GetSpritePreviewInfo(options, AsBaseState(_ppuStateBuffer), AsBaseState(_ppuToolsStateBuffer));
	if(previewInfo.VisibleWidth == 0 || previewInfo.VisibleHeight == 0) {
		return false;
	}

	_spriteMaskCpuType = cpuType;
	_spriteMaskWidth = previewInfo.VisibleWidth;
	_spriteMaskHeight = previewInfo.VisibleHeight;
	_spriteMaskSize = _spriteMaskWidth * _spriteMaskHeight;
	_recordSpriteMask = true;

	MessageManager::DisplayMessage(
		"DataRecording",
		"PPU sprite mask export enabled: " + std::to_string(_spriteMaskWidth) + "x" + std::to_string(_spriteMaskHeight)
	);
	return true;
}

void DataCollector::ReadCombinedMemoryState(MemoryDumper* memoryDumper, MemoryType baseType, MemoryType extType, vector<uint8_t>& output)
{
	if(baseType == MemoryType::None) {
		output.resize(0);
		return;
	}

	uint32_t baseSize = memoryDumper->GetMemorySize(baseType);
	uint32_t extSize = extType != baseType && extType != MemoryType::None ? memoryDumper->GetMemorySize(extType) : 0;
	output.resize(baseSize + extSize);

	if(baseSize > 0) {
		memoryDumper->GetMemoryState(baseType, output.data());
	}
	if(extSize > 0) {
		memoryDumper->GetMemoryState(extType, output.data() + baseSize);
	}
}

bool DataCollector::CaptureSpriteMask(Emulator* emu, uint8_t* output)
{
	memset(output, 0, _spriteMaskSize);

	DebuggerRequest dbgRequest = emu->GetDebugger(false);
	Debugger* debugger = dbgRequest.GetDebugger();
	if(!debugger) {
		return false;
	}

	PpuTools* ppuTools = debugger->GetPpuTools(_spriteMaskCpuType);
	MemoryDumper* memoryDumper = debugger->GetMemoryDumper();
	if(!ppuTools || !memoryDumper) {
		return false;
	}

	debugger->GetPpuState(AsBaseState(_ppuStateBuffer), _spriteMaskCpuType);
	ppuTools->GetPpuToolsState(AsBaseState(_ppuToolsStateBuffer));

	MemoryType vramType = GetVramMemoryType(_spriteMaskCpuType);
	MemoryType vramExtType = GetVramMemoryType(_spriteMaskCpuType, true);
	ReadCombinedMemoryState(memoryDumper, vramType, vramExtType, _spriteVramBuffer);
	if(_spriteVramBuffer.empty()) {
		return false;
	}

	MemoryType spriteRamType = GetSpriteRamMemoryType(_spriteMaskCpuType);
	MemoryType spriteRamExtType = GetSpriteRamMemoryType(_spriteMaskCpuType, true);
	ReadCombinedMemoryState(memoryDumper, spriteRamType, spriteRamExtType, _spriteRamBuffer);

	GetSpritePreviewOptions options = {};
	options.Background = SpriteBackground::Transparent;
	DebugSpritePreviewInfo previewInfo = ppuTools->GetSpritePreviewInfo(options, AsBaseState(_ppuStateBuffer), AsBaseState(_ppuToolsStateBuffer));
	if(previewInfo.Width == 0 || previewInfo.Height == 0 || previewInfo.SpriteCount == 0) {
		return false;
	}

	DebugPaletteInfo palette = ppuTools->GetPaletteInfo({});
	if(palette.ColorCount == 0) {
		return false;
	}

	vector<DebugSpriteInfo> sprites(previewInfo.SpriteCount);
	_spritePreviews.resize((size_t)previewInfo.SpriteCount * SpritePreviewSize);
	_spriteScreenPreview.resize((size_t)previewInfo.Width * previewInfo.Height);
	uint8_t* spriteRam = _spriteRamBuffer.empty() ? nullptr : _spriteRamBuffer.data();
	ppuTools->GetSpriteList(
		options,
		AsBaseState(_ppuStateBuffer),
		AsBaseState(_ppuToolsStateBuffer),
		_spriteVramBuffer.data(),
		spriteRam,
		palette.RgbPalette,
		sprites.data(),
		_spritePreviews.data(),
		_spriteScreenPreview.data()
	);

	uint32_t copyWidth = std::min(_spriteMaskWidth, previewInfo.VisibleWidth);
	uint32_t copyHeight = std::min(_spriteMaskHeight, previewInfo.VisibleHeight);
	for(uint32_t y = 0; y < copyHeight; y++) {
		uint32_t srcY = previewInfo.VisibleY + y;
		if(srcY >= previewInfo.Height) {
			break;
		}

		for(uint32_t x = 0; x < copyWidth; x++) {
			uint32_t srcX = previewInfo.VisibleX + x;
			if(srcX >= previewInfo.Width) {
				break;
			}

			output[y * _spriteMaskWidth + x] = _spriteScreenPreview[srcY * previewInfo.Width + srcX] != 0;
		}
	}

	return true;
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
	offset += _inputBytesPerFrame;

	if(_recordSpriteMask) {
		CaptureSpriteMask(emu, _captureBuffer.data() + offset);
		offset += _spriteMaskSize;
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

	// Periodic save state - defer to instruction boundary (not safe to save mid-instruction)
	if(_options.SaveStateIntervalFrames > 0) {
		_framesSinceLastSaveState++;
		if(_framesSinceLastSaveState >= _options.SaveStateIntervalFrames) {
			_pendingSaveFrameNumber = frameNumber;
			_pendingSaveState = true;
			_framesSinceLastSaveState = 0;
		}
	}
}

void DataCollector::ProcessPendingSaveState(Emulator* emu)
{
	if(_pendingSaveState.exchange(false)) {
		SavePeriodicState(emu, _pendingSaveFrameNumber);
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
	if(_npySpriteMask.is_open()) _npySpriteMask.close();

	MessageManager::DisplayMessage("DataRecording", "Stopped. Frames: " + std::to_string(_recordedFrameCount));
}

bool DataCollector::IsRecording()
{
	return _recording;
}
