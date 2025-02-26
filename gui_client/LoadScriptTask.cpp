/*=====================================================================
LoadScriptTask.cpp
------------------
Copyright Glare Technologies Limited 2022 -
=====================================================================*/
#include "LoadScriptTask.h"


#include "ThreadMessages.h"
#include "WinterShaderEvaluator.h"
#include <ConPrint.h>
#include <PlatformUtils.h>


LoadScriptTask::LoadScriptTask()
{}


LoadScriptTask::~LoadScriptTask()
{}


void LoadScriptTask::run(size_t thread_index)
{
	// conPrint("LoadScriptTask: Loading script...");

	try
	{
		Reference<ScriptLoadedThreadMessage> msg = new ScriptLoadedThreadMessage();

		msg->script = script_content;
		msg->script_evaluator = new WinterShaderEvaluator(base_dir_path, script_content);

		result_msg_queue->enqueue(msg);
	}
	catch(glare::Exception& e)
	{
		result_msg_queue->enqueue(new LogMessage("Error while loading script: " + e.what()));
	}
}
