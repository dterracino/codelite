#include "clang_driver.h"
#include "clang_code_completion.h"
#include <wx/regex.h>
#include "file_logger.h"
#include "pluginmanager.h"
#include "includepathlocator.h"
#include "environmentconfig.h"
#include "tags_options_data.h"
#include "ctags_manager.h"
#include <wx/tokenzr.h>
#include "processreaderthread.h"
#include "manager.h"
#include "project.h"
#include "configuration_mapping.h"
#include "procutils.h"
#include "fileextmanager.h"
#include "globals.h"

static wxString PRE_PROCESS_CMD = wxT("\"$CLANG\" -cc1 $ARGS -w \"$SRC_FILE\" -E 1> \"$PP_OUTPUT_FILE\" 2>&1");
static wxString PCH_CMD         = wxT("\"$CLANG\" -cc1 -x c++-header $ARGS -w \"$SRC_FILE\" -emit-pch -o \"$PCH_FILE\"");
static wxString CC_CMD          = wxT("\"$CLANG\" -cc1 $ARGS -w -fsyntax-only -include-pch \"$PCH_FILE\" -code-completion-at=$LOCATION \"$SRC_FILE\"");

BEGIN_EVENT_TABLE(ClangDriver, wxEvtHandler)
	EVT_COMMAND(wxID_ANY, wxEVT_PROC_DATA_READ,  ClangDriver::OnClangProcessOutput)
	EVT_COMMAND(wxID_ANY, wxEVT_PROC_TERMINATED, ClangDriver::OnClangProcessTerminated)
END_EVENT_TABLE()

static bool WriteFileLatin1(const wxString &file_name, const wxString &content)
{
	wxFFile file(file_name, wxT("wb"));
	if (file.IsOpened() == false) {
		// Nothing to be done
		return false;
	}

	// write the new content
	file.Write(content);
	file.Close();
	return true;
}

#ifdef __WXMSW__
static wxString MSWGetDefaultClangBinary() {
	static bool initialized = false;
	static wxString defaultClang;

	if(!initialized) {
		initialized = true;
		// try to locate the default binary
		wxFileName exePath(wxStandardPaths::Get().GetExecutablePath());
		defaultClang = exePath.GetPath();
		defaultClang << wxFileName::GetPathSeparator() << wxT("clang++.exe");

		if(!wxFileName::FileExists(defaultClang)) {
			defaultClang.Clear();
		}

		if(defaultClang.IsEmpty() == false) {
			CL_SYSTEM(wxT("Located default clang binary: %s"), defaultClang.c_str());

		} else {
			CL_SYSTEM(wxT("Could not locate default clang binary"));

		}
	}

	return defaultClang;
}
#endif

ClangDriver::ClangDriver()
	: m_process(NULL)
	, m_activationPos(wxNOT_FOUND)
	, m_activationEditor(NULL)
	, m_commandType(CT_PreProcess) {
	wxTheApp->Connect(wxEVT_FILE_SAVED, wxCommandEventHandler(ClangDriver::OnFileSaved), NULL, this);
}

ClangDriver::~ClangDriver() {
	wxTheApp->Disconnect(wxEVT_FILE_SAVED, wxCommandEventHandler(ClangDriver::OnFileSaved), NULL, this);
}

void ClangDriver::CodeCompletion(IEditor* editor) {
	
	// Clear the compilation arguments
	m_compilationArgs.Clear();
	CL_DEBUG(wxT(" ==========> ClangDriver::CodeCompletion() started <=============="));

	if(!editor) {
		CL_WARNING(wxT("ClangDriver::CodeCompletion() called with NULL editor!"));
		return;
	}

	if(m_process) {
		// another command is already running
		CL_DEBUG(wxT("Another completion is in progress..."));
		CL_DEBUG(wxT(" ==========> ClangDriver::CodeCompletion() ENDED <=============="));
		return;
	}

	// Start clean..
	DoCleanup();
	const TagsOptionsData &options = TagsManagerST::Get()->GetCtagsOptions();
	if(! (options.GetClangOptions() & CC_CLANG_ENABLED) ) {
		CL_DEBUG1(wxT("clang code-completion is disabled."));
		return ;
	}

	const ClangPCHEntry& entry = m_cache.GetPCH(editor->GetFileName().GetFullPath());

	wxArrayString removedIncludes;
	wxString current_buffer = editor->GetTextRange(0, editor->GetCurrentPosition());
	DoRemoveAllIncludeStatements(current_buffer, removedIncludes);

	bool isValid   = entry.IsValid();
	bool needRegen = entry.NeedRegenration(removedIncludes);

	if(isValid && !needRegen) {
		CL_DEBUG(wxT("Valid PCH cache entry found for file: %s"), editor->GetFileName().GetFullName().c_str());
		CL_DEBUG(wxT("ClangDriver::CodeCompletion(): Calling DoRunCommand with state: CT_CodeCompletion"));
		DoRunCommand(editor, CT_CodeCompletion);

	} else {
		if(isValid) {
			CL_DEBUG(wxT("Regenerating PCH file.."));
		} else {
			CL_DEBUG(wxT("No PCH entry was found for file: "), editor->GetFileName().GetFullName().c_str());
		}

		CL_DEBUG(wxT("ClangDriver::CodeCompletion(): Calling DoRunCommand with state: CT_PreProcess"));
		DoRunCommand(editor, CT_PreProcess);

	}
}

void ClangDriver::OnClangProcessOutput(wxCommandEvent& e) {
	ProcessEventData *ped = (ProcessEventData*) e.GetClientData();
	if(ped) {
		m_output << ped->GetData();
		delete ped;
	}
	e.Skip();
}

void ClangDriver::OnClangProcessTerminated(wxCommandEvent& e) {
	ProcessEventData *ped = (ProcessEventData*) e.GetClientData();
	delete ped;

	// Parse the output
	switch(m_commandType) {
	case CT_PreProcess:
		OnPreProcessingCompleted();
		break;

	case CT_CreatePCH:
		OnPCHCreationCompleted();
		break;

	case CT_CodeCompletion:
		OnCodeCompletionCompleted();
		break;
	}
}

void ClangDriver::DoRunCommand(IEditor* editor, CommandType type) {
	// Sanity:
	if(!editor || !ManagerST::Get()->IsWorkspaceOpen())
		return ;

	// check if clang code-completion is enabled
	ClangDriverCleaner cleaner(this);

	const TagsOptionsData &options = TagsManagerST::Get()->GetCtagsOptions();

	m_commandType = type;

	// Obtain the clang binary name
	wxString clangBinary = options.GetClangBinary();
	clangBinary.Trim().Trim(false);
	
	// Determine which clang binary we use
	if(clangBinary.IsEmpty()) {
#ifdef __WXMSW__
		clangBinary = MSWGetDefaultClangBinary();
#else
		clangBinary = wxT("clang");
#endif
	}
	
	// Prepare the compilation arguments and store them in m_compilationArgs
	DoPrepareCompilationArgs(editor->GetProjectName(), clangBinary);
	
	// First, we need to find the currently active workspace configuration
	wxString currentBuffer = editor->GetTextRange(0, editor->GetCurrentPosition());
	if(currentBuffer.IsEmpty()) {
		return ;
	}

	if(type == CT_PreProcess) {
		// Remove all the include statements from the code
		m_removedIncludes.Clear();
		DoRemoveAllIncludeStatements(currentBuffer, m_removedIncludes);
	}

	wxString filterWord;

	// Move backward until we found our -> or :: or .
	for(size_t i=currentBuffer.Length() - 1; i>0; i--) {
		if(currentBuffer.EndsWith(wxT("->")) || currentBuffer.EndsWith(wxT(".")) || currentBuffer.EndsWith(wxT("::"))) {
			break;
		} else {
			filterWord.Prepend(currentBuffer.GetChar(i));
			currentBuffer.Truncate(currentBuffer.Length() - 1);
		}
	}

	// Get the current line's starting pos
	int lineStartPos = editor->PosFromLine( editor->GetCurrentLine() );
	int column       = editor->GetCurrentPosition() - lineStartPos  + 1;
	int line         = editor->GetCurrentLine() + 1;

	if(type == CT_CodeCompletion) {
		int where = currentBuffer.Find(wxT('\n'), true);
		if(where != wxNOT_FOUND) {
			CL_DEBUG1(wxT("clang code completion has been invoked for the line: %s"), currentBuffer.Mid(where).c_str());
		}
	}

	column -= (int)filterWord.Length();

	// Create temp file
	m_tmpfile.clear();
	m_tmpfile << editor->GetFileName().GetPath(wxPATH_GET_SEPARATOR|wxPATH_GET_VOLUME) << editor->GetFileName().GetName() << wxT("_clang_tmp") << wxT(".cpp");

	// Select the pattern
	wxString command;
	switch(type) {
	case CT_PreProcess:
		command = PRE_PROCESS_CMD;
		break;

	case CT_CreatePCH:
		command = PCH_CMD;
		break;

	case CT_CodeCompletion:
		command = CC_CMD;
		break;
	}

	// Replace the place holders
	wxString ppOutputFile;
	ppOutputFile << DoGetPchHeaderFile(editor->GetFileName().GetFullPath()) << wxT(".1");

	command.Replace(wxT("$CLANG"),          clangBinary);
	command.Replace(wxT("$ARGS"),           m_compilationArgs);
	command.Replace(wxT("$PCH_FILE"),       DoGetPchOutputFileName(editor->GetFileName().GetFullPath()));
	command.Replace(wxT("$PP_OUTPUT_FILE"), ppOutputFile);

	if(type == CT_CreatePCH) {
		// Creating PCH file
		command.Replace(wxT("$SRC_FILE"), DoGetPchHeaderFile(editor->GetFileName().GetFullPath()));

	} else if(type == CT_CodeCompletion ) {

		wxString location;
		wxString completefileName;
		CL_DEBUG(wxT("Preparing input file for clang..."));
		// Prepare the file name
		FileExtManager::FileType type = FileExtManager::GetType(editor->GetFileName().GetFullPath());
		if(type == FileExtManager::TypeSourceC || type == FileExtManager::TypeSourceCpp) {
			
			if(!WriteFileLatin1(m_tmpfile, currentBuffer)){
				CL_ERROR(wxT("Failed to write temp file: %s"), m_tmpfile.c_str());
				return ;
			}

			completefileName << editor->GetFileName().GetName() << wxT("_clang_tmp") << wxT(".cpp");
			location << completefileName << wxT(":") << line << wxT(":") << column << wxT(" ");

		} else {
			wxString implFile;
			implFile << wxT("#include <") << editor->GetFileName().GetFullName() << wxT(">\n");
			if(!WriteFileLatin1(m_tmpfile, implFile)) {
				CL_ERROR(wxT("Failed to write temp file: %s"), implFile.c_str());
				return ;
			}

			completefileName << editor->GetFileName().GetName() << wxT("_clang_tmp") << wxT(".cpp");
			location << editor->GetFileName().GetFullPath() << wxT(":") << line << wxT(":") << column;
		}
		
		command.Replace(wxT("$SRC_FILE"), completefileName);
		command.Replace(wxT("$LOCATION"), location);

	} else if(type == CT_PreProcess) {
		command.Replace(wxT("$SRC_FILE"), editor->GetFileName().GetFullPath());
		WrapInShell(command);
	}

	// Remove any white spaces from the command
	command.Replace(wxT("\n"), wxT(" "));
	command.Replace(wxT("\r"), wxT(" "));

	// Print the command if logging is enabled
	CL_DEBUG(wxT("ClangDriver::DoRunCommand(): %s"), command.c_str());

	// Launch the process
	m_process = CreateAsyncProcess(this, command, IProcessCreateDefault, editor->GetFileName().GetPath(wxPATH_GET_SEPARATOR|wxPATH_GET_VOLUME));
	if(! m_process ) {
		wxLogMessage(wxT("Failed to start process: %s"), command.c_str());
		return ;
	}

	// Reset the cleaner...
	cleaner.Clear();

	m_activationEditor = editor;
	m_activationPos    = lineStartPos + column - 1;
}

wxArrayString ClangDriver::GetStandardIncludePathsArgs(const wxString &clangBinary) {
	static wxArrayString paths, dummy;

	if(paths.IsEmpty() == false)
		return paths;

	IncludePathLocator pathLocator(PluginManager::Get());
	pathLocator.Locate(paths, dummy, clangBinary);

	for(size_t i=0; i<paths.Count(); i++) {
		paths.Item(i).Prepend(wxT("-I"));
	}

	return paths;
}

void ClangDriver::DoCleanup() {
	if(m_process)
		delete m_process;

	m_process = NULL;

	// remove the temporary file
//	if(m_tmpfile.IsEmpty() == false) {
//#ifndef __WXMSW__
//		::unlink( m_tmpfile.mb_str(wxConvUTF8).data() );
//#endif
//		wxRemoveFile( m_tmpfile );
//		m_tmpfile.Clear();
//	}

	m_commandType = CT_PreProcess;
	m_output.Clear();
}

void ClangDriver::OnPCHCreationCompleted() {
	CL_DEBUG(wxT("ClangDriver::OnPCHCreationCompleted() called"));
	CL_DEBUG1(wxT("ClangDriver::OnPCHCreationCompleted():\n[%s]"), m_output.c_str());

	if(m_activationEditor) {
		wxString filename = m_activationEditor->GetFileName().GetFullPath();
		m_cache.AddPCH(filename, DoGetPchOutputFileName(filename), m_removedIncludes, m_pchHeaders);
		CL_DEBUG(wxT("caching PCH file: %s for file %s"), DoGetPchOutputFileName(filename).c_str(), filename.c_str());
		wxRemoveFile(DoGetPchHeaderFile(filename));
	}

	m_pchHeaders.Clear();
	DoCleanup();
	DoRunCommand(m_activationEditor, CT_CodeCompletion);
}

void ClangDriver::OnCodeCompletionCompleted() {
	// clang output is stored in m_output
	wxString output = m_output;
	CL_DEBUG(wxT("ClangDriver::OnCodeCompletionCompleted() called"));
	CL_DEBUG1(wxT("ClangDriver::OnCodeCompletionCompleted():\n[%s]"), output.c_str());

	DoCleanup();
	ClangCodeCompletion::Instance()->DoParseOutput(output);

	CL_DEBUG(wxT(" ==========> ClangDriver::CodeCompletion() ENDED <=============="));
}

void ClangDriver::OnPreProcessingCompleted() {
	if(!m_activationEditor) {
		DoCleanup();
		return;
	}

	CL_DEBUG(wxT("ClangDriver::OnPreProcessingCompleted() calling DoFilterIncludeFilesFromPP()"));
	DoFilterIncludeFilesFromPP();
	CL_DEBUG(wxT("ClangDriver::OnPreProcessingCompleted() calling DoFilterIncludeFilesFromPP() ended"));

	CL_DEBUG(wxT("ClangDriver::OnPreProcessingCompleted() called"));
	CL_DEBUG1(wxT("ClangDriver::OnCodeCompletionCompleted():\n[%s]"), m_output.c_str());

	DoCleanup();

	std::set<wxString> files;
	for(size_t i=0; i<m_pchHeaders.GetCount(); i++) {
		files.insert(m_pchHeaders.Item(i));
	}
	DoRunCommand(m_activationEditor, CT_CreatePCH);
}

void ClangDriver::DoFilterIncludeFilesFromPP() {
	wxString tmpfilename = DoGetPchHeaderFile(m_activationEditor->GetFileName().GetFullPath());
	tmpfilename << wxT(".1");

	wxString content;
	wxString basedir = m_activationEditor->GetFileName().GetPath();

	wxFFile fp(tmpfilename, wxT("rb"));
	if(fp.IsOpened()) {
		fp.ReadAll(&content);
	}
	fp.Close();
	wxRemoveFile(tmpfilename);

	wxArrayString includes;
	wxArrayString lines = wxStringTokenize(content, wxT("\n\r"), wxTOKEN_STRTOK);
	for(size_t i=0; i<lines.GetCount(); i++) {
		wxString curline = lines.Item(i);

		curline.Trim().Trim(false);
		if(curline.IsEmpty())
			continue;

		if(curline.GetChar(0) != wxT('#'))
			continue;

		// an example for a valid line:
		// # 330 "c:\\Users\\eran\\software\\mingw-4.4.1\\include/stdio.h"

		// Trim the dots
		static wxRegEx reIncludeFile(wxT("^#[ \\t]*[0-9]+[ \\t]*\"([a-zA-Z0-9_/\\\\: \\.\\+\\-]+)\""));

		if(!reIncludeFile.Matches(curline))
			continue;

		curline = reIncludeFile.GetMatch(curline, 1);
		curline.Replace(wxT("\\\\"), wxT("\\"));

		bool includeIt = true;
		wxFileName fn(curline);
		switch(FileExtManager::GetType(fn.GetFullName())) {
		case FileExtManager::TypeSourceC:
		case FileExtManager::TypeSourceCpp:
			if(fn.GetFullName() == m_activationEditor->GetFileName().GetFullName()) {
				includeIt = false;
			}
			break;
		default:
			break;
		}

		if(!includeIt) continue;

		fn.Normalize(wxPATH_NORM_ALL & ~wxPATH_NORM_LONG, basedir);
		wxString fullpath = fn.GetFullPath();

		if(includes.Index(fullpath) == wxNOT_FOUND) {
			includes.Add(fullpath);
		}
	}

	wxString pchHeaderFile = DoGetPchHeaderFile(m_activationEditor->GetFileName().GetFullPath());
	wxString pchHeaderFileContent;
	m_pchHeaders.Clear();
	for(size_t i=0; i<includes.GetCount(); i++) {
		if(ShouldInclude(includes.Item(i))) {
			m_pchHeaders.Add(includes.Item(i));
			pchHeaderFileContent << wxT("#include \"") << includes.Item(i) << wxT("\"\n");
		}
	}

	WriteFileWithBackup(pchHeaderFile, pchHeaderFileContent, false);
}

wxString ClangDriver::DoGetPchHeaderFile(const wxString& filename) {
	wxFileName fn(filename);
	wxString name;
	name << ClangPCHCache::GetCacheDirectory()
	     << wxFileName::GetPathSeparator()
	     << fn.GetName()
	     << wxT("__H__.h");
	return name;
}

wxString ClangDriver::DoGetPchOutputFileName(const wxString& filename) {
	return DoGetPchHeaderFile(filename) + wxT(".pch");
}

void ClangDriver::Abort() {
	m_activationEditor = NULL;
	m_activationPos = wxNOT_FOUND;
	m_pchHeaders.Clear();
	m_removedIncludes.Clear();
	DoCleanup();
}

void ClangDriver::DoRemoveAllIncludeStatements(wxString& buffer, wxArrayString &includesRemoved) {
	static wxRegEx reIncludeFile(wxT("^[ \\t]*#[ \\t]*include[ \\t]*[\"\\<]{1}([a-zA-Z0-9_/\\\\: \\.\\+\\-]+)[\"\\>]{1}"));
	wxArrayString lines = wxStringTokenize(buffer, wxT("\n"), wxTOKEN_RET_DELIMS);
	CL_DEBUG(wxT("Calling DoRemoveAllIncludeStatements()"));
	buffer.Clear();
	for(size_t i=0; i<lines.GetCount(); i++) {

		if(i >= 300) break;

		wxString curline = lines.Item(i);
		wxString trimLine = lines.Item(i);
		trimLine.Trim().Trim(false);

		// Optimize: dont use regex unless we are sure that this is a preprocessor line
		if(!trimLine.IsEmpty() && trimLine.GetChar(0) == wxT('#') && reIncludeFile.Matches(curline)) {

			wxString filename = reIncludeFile.GetMatch(curline, 1);
			wxFileName fn(filename);
			includesRemoved.Add(fn.GetFullPath());

			reIncludeFile.ReplaceAll(&curline, wxT(""));
			buffer << curline;

		} else {
			buffer << curline;
		}
	}

	for(size_t i=0; i<includesRemoved.GetCount(); i++) {
		CL_DEBUG(wxT("Stripping include: %s"), includesRemoved.Item(i).c_str());
	}
	CL_DEBUG(wxT("Calling DoRemoveAllIncludeStatements()- ENDED"));
}

bool ClangDriver::ShouldInclude(const wxString& header) {
	// Header is in the form of full path
	for(size_t i=0; i<m_removedIncludes.GetCount(); i++) {
		if(header.EndsWith(m_removedIncludes.Item(i))) {
			return true;
		}
	}
	return false;
}

void ClangDriver::OnFileSaved(wxCommandEvent& e) {
	e.Skip();
}

void ClangDriver::DoPrepareCompilationArgs(const wxString& projectName, const wxString &clangBinary) {
	if(m_compilationArgs.IsEmpty() == false) {
		return;
	}
		
	wxString binary = clangBinary;
#ifndef __WXMSW__
	if(binary.IsEmpty() == false) {
		binary << wxT(" -cc1 ");
	}
#else
	// since we install the binary outselves, we use the
	// paths as seen by g++
	binary.Clear();
#endif
	
	wxArrayString args;
	args = GetStandardIncludePathsArgs(binary);
	
	wxString errMsg;
	BuildMatrixPtr matrix = WorkspaceST::Get()->GetBuildMatrix();
	if(!matrix) {
		return;
	}

	wxString workspaceSelConf = matrix->GetSelectedConfigurationName();

	// Now that we got the selected workspace configuration, extract the related project configuration
	ProjectPtr proj =  WorkspaceST::Get()->FindProjectByName(projectName, errMsg);
	if(!proj) {
		return;
	}

	wxString projectSelConf = matrix->GetProjectSelectedConf(workspaceSelConf, proj->GetName());
	BuildConfigPtr dependProjbldConf = WorkspaceST::Get()->GetProjBuildConf(proj->GetName(), projectSelConf);

	// no build config?
	if(!dependProjbldConf)
		return;
	
	// no support for custom projects yet
	if(dependProjbldConf->IsCustomBuild())
		return;

	// Get the include paths and add them
	wxString projectIncludePaths = dependProjbldConf->GetIncludePath();
	wxArrayString projectIncludePathsArr = wxStringTokenize(projectIncludePaths, wxT(";"), wxTOKEN_STRTOK);
	for(size_t i=0; i<projectIncludePathsArr.GetCount(); i++) {
		args.Add( wxString::Format(wxT("-I%s"), projectIncludePathsArr[i].c_str()) );
	}
	
	// get the compiler options and add them
	wxString projectCompileOptions = dependProjbldConf->GetCompileOptions();
	wxArrayString projectCompileOptionsArr = wxStringTokenize(projectCompileOptions, wxT(";"), wxTOKEN_STRTOK);
	for(size_t i=0; i<projectCompileOptionsArr.GetCount(); i++) {
		wxString cmpOption (projectCompileOptionsArr.Item(i));
		cmpOption.Trim().Trim(false);
		wxString tmp;
		// Expand backticks / $(shell ...) syntax supported by codelite
		if(cmpOption.StartsWith(wxT("$(shell "), &tmp) || cmpOption.StartsWith(wxT("`"), &tmp)) {
			cmpOption = tmp;
			tmp.Clear();
			if(cmpOption.EndsWith(wxT(")"), &tmp) || cmpOption.EndsWith(wxT("`"), &tmp)) {
				cmpOption = tmp;
			}
			if(m_backticks.find(cmpOption) == m_backticks.end()) {
				// Expand the backticks into their value
				wxArrayString outArr;
				// Apply the environment before executing the command
				EnvSetter setter( EnvironmentConfig::Instance() );
				ProcUtils::SafeExecuteCommand(cmpOption, outArr);
				wxString expandedValue;
				for(size_t j=0; j<outArr.size(); j++) {
					expandedValue << outArr.Item(j) << wxT(" ");
				}
				m_backticks[cmpOption] = expandedValue;
				cmpOption = expandedValue;
			} else {
				cmpOption = m_backticks.find(cmpOption)->second;
			}
		}
		args.Add( cmpOption );
	}
	// get the compiler preprocessor and add them as well
	wxString projectPreps = dependProjbldConf->GetPreprocessor();
	wxArrayString projectPrepsArr = wxStringTokenize(projectPreps, wxT(";"), wxTOKEN_STRTOK);
	for(size_t i=0; i<projectPrepsArr.GetCount(); i++) {
		args.Add( wxString::Format(wxT("-D%s"), projectPrepsArr[i].c_str()) );
	}
	
	// Add the arguments
	m_compilationArgs.Clear();
	for(size_t i=0; i<args.size(); i++) {
		m_compilationArgs << wxT(" ") << args.Item(i);
	}
	
	// Remove some of the flags which are known to cause problems to clang under Windows
	m_compilationArgs.Replace(wxT("-fno-strict-aliasing"), wxT(""));
	m_compilationArgs.Replace(wxT("-mthreads"),            wxT(""));
	m_compilationArgs.Replace(wxT("-pipe"),                wxT(""));
	m_compilationArgs.Replace(wxT("-fmessage-length=0"),   wxT(""));
	m_compilationArgs.Replace(wxT("-g"),                   wxT(""));
	m_compilationArgs.Replace(wxT("-fPIC"),                wxT(""));
	
	CL_DEBUG(wxT("Using compilation args: %s"), m_compilationArgs.c_str());
}
