#include "DynamicMapping.h"
#include <Psapi.h>
#include "ntdll.h"

#pragma comment(lib, "psapi.lib")

LPVOID MapModuleToProcess(HANDLE hProcess, BYTE * dllMemory)
{
	DWORD_PTR dwDelta = 0;
	PIMAGE_DOS_HEADER pDosHeader = (PIMAGE_DOS_HEADER)dllMemory;
	PIMAGE_NT_HEADERS pNtHeader = (PIMAGE_NT_HEADERS)((DWORD_PTR)pDosHeader + pDosHeader->e_lfanew);
	PIMAGE_SECTION_HEADER pSecHeader = IMAGE_FIRST_SECTION(pNtHeader);

	if (pDosHeader->e_magic != IMAGE_DOS_SIGNATURE || pNtHeader->Signature != IMAGE_NT_SIGNATURE)
	{
		return 0;
	}

	if (!pNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress)
	{
		return 0;
	}

	LPVOID imageRemote = VirtualAllocEx(hProcess, 0, pNtHeader->OptionalHeader.SizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	LPVOID imageLocal = VirtualAlloc(0, pNtHeader->OptionalHeader.SizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

	if (!imageLocal || !imageRemote)
	{
		return 0;
	}

	dwDelta = (DWORD_PTR)imageRemote - pNtHeader->OptionalHeader.ImageBase;

	memcpy((LPVOID)imageLocal, (LPVOID)pDosHeader, pNtHeader->OptionalHeader.SizeOfHeaders);

	for (WORD i = 0; i < pNtHeader->FileHeader.NumberOfSections; i++)
	{
		memcpy((LPVOID)((DWORD_PTR)imageLocal + pSecHeader->VirtualAddress), (LPVOID)((DWORD_PTR)pDosHeader + pSecHeader->PointerToRawData), pSecHeader->SizeOfRawData);
		pSecHeader++;
	}

	DoBaseRelocation(
		(PIMAGE_BASE_RELOCATION)((DWORD_PTR)imageLocal + pNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress),
		(DWORD_PTR)imageLocal,
		dwDelta);

	if (WriteProcessMemory(hProcess, imageRemote, imageLocal, pNtHeader->OptionalHeader.SizeOfImage, 0))
	{
		VirtualFree(imageLocal, 0, MEM_RELEASE);
		return imageRemote;
	}
	else
	{
		VirtualFree(imageLocal, 0, MEM_RELEASE);
		VirtualFreeEx(hProcess, imageRemote, 0, MEM_RELEASE);
		return 0;
	}
}

void DoBaseRelocation(PIMAGE_BASE_RELOCATION relocation, DWORD_PTR memory, DWORD_PTR dwDelta)
{
	DWORD_PTR * patchAddress;
	WORD type, offset;

	while (relocation->VirtualAddress)
	{
		PBYTE dest = (PBYTE)(memory + relocation->VirtualAddress);
		DWORD count = (relocation->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
		WORD * relocInfo = (WORD *)((DWORD_PTR)relocation + sizeof(IMAGE_BASE_RELOCATION));

		for (DWORD i = 0; i < count; i++)
		{
			type = relocInfo[i] >> 12;
			offset = relocInfo[i] & 0xfff;

			switch (type)
			{
			case IMAGE_REL_BASED_ABSOLUTE:
				break;
			case IMAGE_REL_BASED_HIGHLOW:
			case IMAGE_REL_BASED_DIR64:
				patchAddress = (DWORD_PTR *)(dest + offset);
				*patchAddress += dwDelta;
				break;
			default:
				break;
			}
		}

		relocation = (PIMAGE_BASE_RELOCATION)((DWORD_PTR)relocation + relocation->SizeOfBlock);
	}
}

DWORD RVAToOffset(PIMAGE_NT_HEADERS pNtHdr, DWORD dwRVA)
{
	PIMAGE_SECTION_HEADER pSectionHdr = IMAGE_FIRST_SECTION(pNtHdr);

	for (WORD i = 0; i < pNtHdr->FileHeader.NumberOfSections; i++)
	{
		if (pSectionHdr->VirtualAddress <= dwRVA)
		{
			if ((pSectionHdr->VirtualAddress + pSectionHdr->Misc.VirtualSize) > dwRVA)
			{
				dwRVA -= pSectionHdr->VirtualAddress;
				dwRVA += pSectionHdr->PointerToRawData;

				return (dwRVA);
			}

		}
		pSectionHdr++;
	}

	return (0);
}

DWORD GetDllFunctionAddressRVA(BYTE * dllMemory, LPCSTR apiName)
{
	PIMAGE_DOS_HEADER pDosHeader = (PIMAGE_DOS_HEADER)dllMemory;
	PIMAGE_NT_HEADERS pNtHeader = (PIMAGE_NT_HEADERS)((DWORD_PTR)pDosHeader + pDosHeader->e_lfanew);
	PIMAGE_EXPORT_DIRECTORY pExportDir;

	DWORD exportDirRVA = pNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
	DWORD exportDirOffset = RVAToOffset(pNtHeader, exportDirRVA);

	pExportDir = (PIMAGE_EXPORT_DIRECTORY)((DWORD_PTR)dllMemory + exportDirOffset);

	DWORD * addressOfFunctionsArray = (DWORD *)((DWORD)pExportDir->AddressOfFunctions - exportDirRVA + (DWORD_PTR)pExportDir);
	DWORD * addressOfNamesArray = (DWORD *)((DWORD)pExportDir->AddressOfNames - exportDirRVA + (DWORD_PTR)pExportDir);
	WORD * addressOfNameOrdinalsArray = (WORD *)((DWORD)pExportDir->AddressOfNameOrdinals - exportDirRVA + (DWORD_PTR)pExportDir);

	for (DWORD i = 0; i < pExportDir->NumberOfNames; i++)
	{
		char * functionName = (char*)(addressOfNamesArray[i] - exportDirRVA + (DWORD_PTR)pExportDir);

		if (!_stricmp(functionName, apiName))
		{
			return addressOfFunctionsArray[addressOfNameOrdinalsArray[i]];
		}
	}

	return 0;
}

HMODULE GetModuleBaseRemote(HANDLE hProcess, const wchar_t* szDLLName)
{
	DWORD cbNeeded = 0;
	wchar_t szModuleName[MAX_PATH] = { 0 };
	if (EnumProcessModules(hProcess, 0, 0, &cbNeeded))
	{
		HMODULE* hMods = (HMODULE*)malloc(cbNeeded*sizeof(HMODULE));
		if (EnumProcessModules(hProcess, hMods, cbNeeded, &cbNeeded))
		{
			for (unsigned int i = 0; i < cbNeeded / sizeof(HMODULE); i++)
			{
				szModuleName[0] = 0;
				if (GetModuleFileNameExW(hProcess, hMods[i], szModuleName, _countof(szModuleName)))
				{
					wchar_t* dllName = wcsrchr(szModuleName, L'\\');
					if (dllName)
					{
						dllName++;
						if (!_wcsicmp(dllName, szDLLName))
						{
							return hMods[i];
						}
					}
				}
			}
		}
		free(hMods);
	}
	return 0;
}

DWORD StartDllInitFunction(HANDLE hProcess, DWORD_PTR functionAddress, LPVOID imageBase)
{
	DWORD dwExit = 0;
	HANDLE hThread = CreateRemoteThread(hProcess, 0, 0, (LPTHREAD_START_ROUTINE)functionAddress, imageBase, CREATE_SUSPENDED, 0);
	if (hThread)
	{
		NTSTATUS ntStat = NtSetInformationThread(hThread, ThreadHideFromDebugger, 0, 0);
		ResumeThread(hThread);

		WaitForSingleObject(hThread, INFINITE);
		GetExitCodeThread(hThread, &dwExit);

		CloseHandle(hThread);
		return dwExit;
	}

	return -1;
}