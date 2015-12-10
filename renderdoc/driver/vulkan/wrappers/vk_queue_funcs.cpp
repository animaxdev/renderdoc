/******************************************************************************
 * The MIT License (MIT)
 * 
 * Copyright (c) 2015 Baldur Karlsson
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "../vk_core.h"

bool WrappedVulkan::Serialise_vkGetDeviceQueue(
		Serialiser*                                 localSerialiser,
    VkDevice                                    device,
    uint32_t                                    queueFamilyIndex,
    uint32_t                                    queueIndex,
    VkQueue*                                    pQueue)
{
	SERIALISE_ELEMENT(ResourceId, devId, GetResID(device));
	SERIALISE_ELEMENT(uint32_t, familyIdx, queueFamilyIndex);
	SERIALISE_ELEMENT(uint32_t, idx, queueIndex);
	SERIALISE_ELEMENT(ResourceId, queueId, GetResID(*pQueue));

	if(m_State == READING)
	{
		device = GetResourceManager()->GetLiveHandle<VkDevice>(devId);

		VkQueue queue;
		ObjDisp(device)->GetDeviceQueue(Unwrap(device), familyIdx, idx, &queue);

		VkQueue real = queue;

		GetResourceManager()->WrapResource(Unwrap(device), queue);
		GetResourceManager()->AddLiveResource(queueId, queue);

		if(familyIdx == m_QueueFamilyIdx)
		{
			m_Queue = queue;
			
			// we can now submit any cmds that were queued (e.g. from creating debug
			// manager on vkCreateDevice)
			SubmitCmds();
		}
	}

	return true;
}

void WrappedVulkan::vkGetDeviceQueue(
    VkDevice                                    device,
    uint32_t                                    queueFamilyIndex,
    uint32_t                                    queueIndex,
    VkQueue*                                    pQueue)
{
	ObjDisp(device)->GetDeviceQueue(Unwrap(device), queueFamilyIndex, queueIndex, pQueue);

	{
		// it's perfectly valid for enumerate type functions to return the same handle
		// each time. If that happens, we will already have a wrapper created so just
		// return the wrapped object to the user and do nothing else
		if(GetResourceManager()->HasWrapper(ToTypedHandle(*pQueue)))
		{
			*pQueue = (VkQueue)GetResourceManager()->GetWrapper(ToTypedHandle(*pQueue));
		}
		else
		{
			ResourceId id = GetResourceManager()->WrapResource(Unwrap(device), *pQueue);

			if(m_State >= WRITING)
			{
				Chunk *chunk = NULL;

				{
					CACHE_THREAD_SERIALISER();

					SCOPED_SERIALISE_CONTEXT(GET_DEVICE_QUEUE);
					Serialise_vkGetDeviceQueue(localSerialiser, device, queueFamilyIndex, queueIndex, pQueue);

					chunk = scope.Get();
				}

				VkResourceRecord *record = GetResourceManager()->AddResourceRecord(*pQueue);
				RDCASSERT(record);

				VkResourceRecord *instrecord = GetRecord(m_Instance);

				// treat queues as pool members of the instance (ie. freed when the instance dies)
				{
					instrecord->LockChunks();
					instrecord->pooledChildren.push_back(record);
					instrecord->UnlockChunks();
				}

				record->AddChunk(chunk);
			}
			else
			{
				GetResourceManager()->AddLiveResource(id, *pQueue);
			}

			if(queueFamilyIndex == m_QueueFamilyIdx)
			{
				m_Queue = *pQueue;

				// we can now submit any cmds that were queued (e.g. from creating debug
				// manager on vkCreateDevice)
				SubmitCmds();
			}
		}
	}
}

bool WrappedVulkan::Serialise_vkQueueSubmit(
		Serialiser*                                 localSerialiser,
    VkQueue                                     queue,
		uint32_t                                    submitCount,
		const VkSubmitInfo*                         pSubmits,
		VkFence                                     fence)
{
	SERIALISE_ELEMENT(ResourceId, queueId, GetResID(queue));
	SERIALISE_ELEMENT(ResourceId, fenceId, fence != VK_NULL_HANDLE ? GetResID(fence) : ResourceId());

	SERIALISE_ELEMENT(uint32_t, numCmds, pSubmits->commandBufferCount);

	vector<ResourceId> cmdIds;
	VkCommandBuffer *cmds = m_State >= WRITING ? NULL : new VkCommandBuffer[numCmds];
	for(uint32_t i=0; i < numCmds; i++)
	{
		ResourceId bakedId;

		if(m_State >= WRITING)
		{
			VkResourceRecord *record = GetRecord(pSubmits->pCommandBuffers[i]);
			RDCASSERT(record->bakedCommands);
			if(record->bakedCommands)
				bakedId = record->bakedCommands->GetResourceID();
		}

		SERIALISE_ELEMENT(ResourceId, id, bakedId);

		if(m_State < WRITING)
		{
			cmdIds.push_back(id);

			cmds[i] = id != ResourceId()
				? Unwrap(GetResourceManager()->GetLiveHandle<VkCommandBuffer>(id))
				: NULL;
		}
	}
	
	if(m_State < WRITING)
	{
		queue = GetResourceManager()->GetLiveHandle<VkQueue>(queueId);
		if(fenceId != ResourceId())
			fence = GetResourceManager()->GetLiveHandle<VkFence>(fenceId);
		else
			fence = VK_NULL_HANDLE;
	}
	
	// we don't serialise semaphores at all, just whether we waited on any.
	// For waiting semaphores, since we don't track state we have to just conservatively
	// wait for queue idle. Since we do that, there's equally no point in signalling semaphores
	SERIALISE_ELEMENT(uint32_t, numWaitSems, pSubmits->waitSemaphoreCount);

	if(m_State < WRITING && numWaitSems > 0)
		ObjDisp(queue)->QueueWaitIdle(Unwrap(queue));

	VkSubmitInfo submitInfo = {
		VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL,
		0, NULL, // wait semaphores
		numCmds, cmds, // command buffers
		0, NULL, // signal semaphores
	};
	
	const string desc = localSerialiser->GetDebugStr();

	if(m_State == READING)
	{
		ObjDisp(queue)->QueueSubmit(Unwrap(queue), 1, &submitInfo, Unwrap(fence));

		for(uint32_t i=0; i < numCmds; i++)
		{
			ResourceId cmd = GetResourceManager()->GetLiveID(cmdIds[i]);
			GetResourceManager()->ApplyBarriers(m_BakedCmdBufferInfo[cmd].imgbarriers, m_ImageLayouts);
		}

		AddEvent(QUEUE_SUBMIT, desc);
		string name = "vkQueueSubmit(" +
						ToStr::Get(numCmds) + ")";

		FetchDrawcall draw;
		draw.name = name;

		draw.flags |= eDraw_PushMarker;

		AddDrawcall(draw, true);

		// add command buffer draws under here
		GetDrawcallStack().push_back(&GetDrawcallStack().back()->children.back());

		m_RootEventID++;

		for(uint32_t c=0; c < numCmds; c++)
		{
			string name = "[" + ToStr::Get(cmdIds[c]) + "]";

			AddEvent(QUEUE_SUBMIT, "cmd " + name);

			FetchDrawcall draw;
			draw.name = name;

			draw.flags |= eDraw_PushMarker;

			AddDrawcall(draw, true);

			DrawcallTreeNode &d = GetDrawcallStack().back()->children.back();

			// copy DrawcallTreeNode children
			d.children = m_BakedCmdBufferInfo[cmdIds[c]].draw->children;

			// assign new event and drawIDs
			RefreshIDs(d.children, m_RootEventID, m_RootDrawcallID);

			m_PartialReplayData.cmdBufferSubmits[cmdIds[c]].push_back(m_RootEventID);

			// 1 extra for the [0] virtual event for the command buffer
			m_RootEventID += 1+m_BakedCmdBufferInfo[cmdIds[c]].eventCount;
			m_RootDrawcallID += m_BakedCmdBufferInfo[cmdIds[c]].drawCount;
		}

		// the outer loop will increment the event ID but we've handled
		// it ourselves, so 'undo' that.
		m_RootEventID--;

		// done adding command buffers
		m_DrawcallStack.pop_back();
	}
	else if(m_State == EXECUTING)
	{
		m_RootEventID++;

		uint32_t startEID = m_RootEventID;

		// advance m_CurEventID to match the events added when reading
		for(uint32_t c=0; c < numCmds; c++)
		{
			// 1 extra for the [0] virtual event for the command buffer
			m_RootEventID += 1+m_BakedCmdBufferInfo[cmdIds[c]].eventCount;
			m_RootDrawcallID += m_BakedCmdBufferInfo[cmdIds[c]].drawCount;
		}

		m_RootEventID--;

		if(m_LastEventID == startEID)
		{
			RDCDEBUG("Queue Submit no replay %u == %u", m_LastEventID, startEID);
		}
		else if(m_LastEventID > startEID && m_LastEventID < m_RootEventID)
		{
			RDCDEBUG("Queue Submit partial replay %u < %u", m_LastEventID, m_RootEventID);

			uint32_t eid = startEID;

			vector<ResourceId> trimmedCmdIds;
			vector<VkCommandBuffer> trimmedCmds;

			for(uint32_t c=0; c < numCmds; c++)
			{
				uint32_t end = eid + m_BakedCmdBufferInfo[cmdIds[c]].eventCount;

				if(eid == m_PartialReplayData.baseEvent)
				{
					ResourceId partial = GetResID(PartialCmdBuf());
					RDCDEBUG("Queue Submit partial replay of %llu at %u, using %llu", cmdIds[c], eid, partial);
					trimmedCmdIds.push_back(partial);
					trimmedCmds.push_back(Unwrap(PartialCmdBuf()));
				}
				else if(m_LastEventID >= end)
				{
					RDCDEBUG("Queue Submit full replay %llu", cmdIds[c]);
					trimmedCmdIds.push_back(cmdIds[c]);
					trimmedCmds.push_back(Unwrap(GetResourceManager()->GetLiveHandle<VkCommandBuffer>(cmdIds[c])));
				}
				else
				{
					RDCDEBUG("Queue not submitting %llu", cmdIds[c]);
				}

				eid += 1+m_BakedCmdBufferInfo[cmdIds[c]].eventCount;
			}

			RDCASSERT(trimmedCmds.size() > 0);
			
			submitInfo.commandBufferCount = (uint32_t)trimmedCmds.size();
			submitInfo.pCommandBuffers = &trimmedCmds[0];
			ObjDisp(queue)->QueueSubmit(Unwrap(queue), 1, &submitInfo, Unwrap(fence));

			for(uint32_t i=0; i < trimmedCmdIds.size(); i++)
			{
				ResourceId cmd = trimmedCmdIds[i];
				GetResourceManager()->ApplyBarriers(m_BakedCmdBufferInfo[cmd].imgbarriers, m_ImageLayouts);
			}
		}
		else
		{
			ObjDisp(queue)->QueueSubmit(Unwrap(queue), 1, &submitInfo, Unwrap(fence));

			for(uint32_t i=0; i < numCmds; i++)
			{
				ResourceId cmd = GetResourceManager()->GetLiveID(cmdIds[i]);
				GetResourceManager()->ApplyBarriers(m_BakedCmdBufferInfo[cmd].imgbarriers, m_ImageLayouts);
			}
		}
	}

	SAFE_DELETE_ARRAY(cmds);

	return true;
}

void WrappedVulkan::RefreshIDs(vector<DrawcallTreeNode> &nodes, uint32_t baseEventID, uint32_t baseDrawID)
{
	// assign new drawcall IDs
	for(size_t i=0; i < nodes.size(); i++)
	{
		nodes[i].draw.eventID += baseEventID;
		nodes[i].draw.drawcallID += baseDrawID;

		for(int32_t e=0; e < nodes[i].draw.events.count; e++)
		{
			nodes[i].draw.events[e].eventID += baseEventID;
			m_Events.push_back(nodes[i].draw.events[e]);
		}

		RefreshIDs(nodes[i].children, baseEventID, baseDrawID);
	}
}

VkResult WrappedVulkan::vkQueueSubmit(
    VkQueue                                     queue,
		uint32_t                                    submitCount,
		const VkSubmitInfo*                         pSubmits,
		VkFence                                     fence)
{
	size_t tempmemSize = sizeof(VkSubmitInfo)*submitCount;
	
	// need to count how many semaphore and command buffer arrays to allocate for
	for(uint32_t i=0; i < submitCount; i++)
	{
		tempmemSize += pSubmits[i].commandBufferCount*sizeof(VkCommandBuffer);
		tempmemSize += pSubmits[i].signalSemaphoreCount*sizeof(VkSemaphore);
		tempmemSize += pSubmits[i].waitSemaphoreCount*sizeof(VkSemaphore);
	}

	byte *memory = GetTempMemory(tempmemSize);

	VkSubmitInfo *unwrappedSubmits = (VkSubmitInfo *)memory;
	VkCommandBuffer *unwrappedObjects = (VkCommandBuffer *)(unwrappedSubmits + submitCount);

	for(uint32_t i=0; i < submitCount; i++)
	{
		RDCASSERT(pSubmits[i].sType == VK_STRUCTURE_TYPE_SUBMIT_INFO && pSubmits[i].pNext == NULL);
		unwrappedSubmits[i] = *pSubmits;

		unwrappedSubmits[i].pWaitSemaphores = unwrappedSubmits[i].waitSemaphoreCount ? (VkSemaphore *)unwrappedObjects : NULL;
		VkSemaphore *sems = (VkSemaphore *)unwrappedObjects;
		for(uint32_t o=0; o < unwrappedSubmits[i].waitSemaphoreCount; o++)
			sems[o] = Unwrap(pSubmits[i].pWaitSemaphores[o]);
		unwrappedObjects += unwrappedSubmits[i].waitSemaphoreCount;

		unwrappedSubmits[i].pCommandBuffers = unwrappedSubmits[i].commandBufferCount ? (VkCommandBuffer *)unwrappedObjects : NULL;
		for(uint32_t o=0; o < unwrappedSubmits[i].commandBufferCount; o++)
			unwrappedObjects[o] = Unwrap(pSubmits[i].pCommandBuffers[o]);
		unwrappedObjects += unwrappedSubmits[i].commandBufferCount;

		unwrappedSubmits[i].pSignalSemaphores = unwrappedSubmits[i].signalSemaphoreCount ? (VkSemaphore *)unwrappedObjects : NULL;
		sems = (VkSemaphore *)unwrappedObjects;
		for(uint32_t o=0; o < unwrappedSubmits[i].signalSemaphoreCount; o++)
			sems[o] = Unwrap(pSubmits[i].pSignalSemaphores[o]);
		unwrappedObjects += unwrappedSubmits[i].signalSemaphoreCount;
	}

	VkResult ret = ObjDisp(queue)->QueueSubmit(Unwrap(queue), submitCount, unwrappedSubmits, Unwrap(fence));

	bool capframe = false;
	set<ResourceId> refdIDs;

	for(uint32_t s=0; s < submitCount; s++)
	{
		for(uint32_t i=0; i < pSubmits[s].commandBufferCount; i++)
		{
			ResourceId cmd = GetResID(pSubmits[s].pCommandBuffers[i]);

			VkResourceRecord *record = GetRecord(pSubmits[s].pCommandBuffers[i]);

			{
				SCOPED_LOCK(m_ImageLayoutsLock);
				GetResourceManager()->ApplyBarriers(record->bakedCommands->cmdInfo->imgbarriers, m_ImageLayouts);
			}

			// need to lock the whole section of code, not just the check on
			// m_State, as we also need to make sure we don't check the state,
			// start marking dirty resources then while we're doing so the
			// state becomes capframe.
			// the next sections where we mark resources referenced and add
			// the submit chunk to the frame record don't have to be protected.
			// Only the decision of whether we're inframe or not, and marking
			// dirty.
			{
				SCOPED_LOCK(m_CapTransitionLock);
				if(m_State == WRITING_CAPFRAME)
				{
					for(auto it = record->bakedCommands->cmdInfo->dirtied.begin(); it != record->bakedCommands->cmdInfo->dirtied.end(); ++it)
						GetResourceManager()->MarkPendingDirty(*it);

					capframe = true;
				}
				else
				{
					for(auto it = record->bakedCommands->cmdInfo->dirtied.begin(); it != record->bakedCommands->cmdInfo->dirtied.end(); ++it)
						GetResourceManager()->MarkDirtyResource(*it);
				}
			}

			if(capframe)
			{
				// for each bound descriptor set, mark it referenced as well as all resources currently bound to it
				for(auto it = record->bakedCommands->cmdInfo->boundDescSets.begin(); it != record->bakedCommands->cmdInfo->boundDescSets.end(); ++it)
				{
					GetResourceManager()->MarkResourceFrameReferenced(GetResID(*it), eFrameRef_Read);

					VkResourceRecord *setrecord = GetRecord(*it);

					for(auto refit = setrecord->descInfo->bindFrameRefs.begin(); refit != setrecord->descInfo->bindFrameRefs.end(); ++refit)
					{
						refdIDs.insert(refit->first);
						GetResourceManager()->MarkResourceFrameReferenced(refit->first, refit->second.second);

						if(refit->second.first & DescriptorSetData::SPARSE_REF_BIT)
						{
							VkResourceRecord *record = GetResourceManager()->GetResourceRecord(refit->first);

							GetResourceManager()->MarkSparseMapReferenced(record->sparseInfo);
						}
					}
				}
				
				for(auto it = record->bakedCommands->cmdInfo->sparse.begin(); it != record->bakedCommands->cmdInfo->sparse.end(); ++it)
					GetResourceManager()->MarkSparseMapReferenced(*it);

				// pull in frame refs from this baked command buffer
				record->bakedCommands->AddResourceReferences(GetResourceManager());
				record->bakedCommands->AddReferencedIDs(refdIDs);

				// ref the parent command buffer by itself, this will pull in the cmd buffer pool
				GetResourceManager()->MarkResourceFrameReferenced(record->GetResourceID(), eFrameRef_Read);

				for(size_t i=0; i < record->bakedCommands->cmdInfo->subcmds.size(); i++)
				{
					record->bakedCommands->cmdInfo->subcmds[i]->bakedCommands->AddResourceReferences(GetResourceManager());
					record->bakedCommands->cmdInfo->subcmds[i]->bakedCommands->AddReferencedIDs(refdIDs);
					GetResourceManager()->MarkResourceFrameReferenced(record->bakedCommands->cmdInfo->subcmds[i]->GetResourceID(), eFrameRef_Read);

					record->bakedCommands->cmdInfo->subcmds[i]->bakedCommands->AddRef();
				}
				
				GetResourceManager()->MarkResourceFrameReferenced(GetResID(queue), eFrameRef_Read);

				if(fence != VK_NULL_HANDLE)
					GetResourceManager()->MarkResourceFrameReferenced(GetResID(fence), eFrameRef_Read);

				{
					SCOPED_LOCK(m_CmdBufferRecordsLock);
					m_CmdBufferRecords.push_back(record->bakedCommands);
					for(size_t i=0; i < record->bakedCommands->cmdInfo->subcmds.size(); i++)
						m_CmdBufferRecords.push_back(record->bakedCommands->cmdInfo->subcmds[i]->bakedCommands);
				}

				record->bakedCommands->AddRef();
			}

			record->cmdInfo->dirtied.clear();
		}
	}
	
	if(capframe)
	{
		vector<VkResourceRecord*> maps;
		{
			SCOPED_LOCK(m_CoherentMapsLock);
			maps = m_CoherentMaps;
		}
		
		for(auto it = maps.begin(); it != maps.end(); ++it)
		{
			VkResourceRecord *record = *it;
			MemMapState &state = *record->memMapState;

			// potential persistent map
			if(state.mapCoherent && state.mappedPtr && !state.mapFlushed)
			{
				// only need to flush memory that could affect this submitted batch of work
				if(refdIDs.find(record->GetResourceID()) == refdIDs.end())
				{
					RDCDEBUG("Map of memory %llu not referenced in this queue - not flushing", record->GetResourceID());
					continue;
				}

				size_t diffStart = 0, diffEnd = 0;
				bool found = true;

				// if we have a previous set of data, compare.
				// otherwise just serialise it all
				if(state.refData)
					found = FindDiffRange((byte *)state.mappedPtr, state.refData, (size_t)state.mapSize, diffStart, diffEnd);
				else
					diffEnd = (size_t)state.mapSize;

				if(found)
				{
					// MULTIDEVICE should find the device for this queue.
					// MULTIDEVICE only want to flush maps associated with this queue
					VkDevice dev = GetDev();

					{
						RDCLOG("Persistent map flush forced for %llu (%llu -> %llu)", record->GetResourceID(), (uint64_t)diffStart, (uint64_t)diffEnd);
						VkMappedMemoryRange range = {
							VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL,
							(VkDeviceMemory)(uint64_t)record->Resource,
							state.mapOffset+diffStart, diffEnd-diffStart
						};
						vkFlushMappedMemoryRanges(dev, 1, &range);
						state.mapFlushed = false;
					}

					GetResourceManager()->MarkPendingDirty(record->GetResourceID());

					// allocate ref data so we can compare next time to minimise serialised data
					if(state.refData == NULL)
						state.refData = Serialiser::AllocAlignedBuffer((size_t)state.mapSize, 64);
					memcpy(state.refData, state.mappedPtr, (size_t)state.mapSize);
				}
				else
				{
					RDCDEBUG("Persistent map flush not needed for %llu", record->GetResourceID());
				}
			}
		}

		{
			CACHE_THREAD_SERIALISER();

			for(uint32_t s=0; s < submitCount; s++)
			{
				SCOPED_SERIALISE_CONTEXT(QUEUE_SUBMIT);
				Serialise_vkQueueSubmit(localSerialiser, queue, 1, &pSubmits[s], fence);

				m_FrameCaptureRecord->AddChunk(scope.Get());
				
				for(uint32_t sem=0; sem < pSubmits[s].waitSemaphoreCount; sem++)
					GetResourceManager()->MarkResourceFrameReferenced(GetResID(pSubmits[s].pWaitSemaphores[sem]), eFrameRef_Read);

				for(uint32_t sem=0; sem < pSubmits[s].signalSemaphoreCount; sem++)
					GetResourceManager()->MarkResourceFrameReferenced(GetResID(pSubmits[s].pSignalSemaphores[sem]), eFrameRef_Read);
			}
		}
	}
	
	return ret;
}

#if 0
bool WrappedVulkan::Serialise_vkQueueBindSparse(
	Serialiser*                                 localSerialiser,
	VkQueue                                     queue,
	uint32_t                                    bindInfoCount,
	const VkBindSparseInfo*                     pBindInfo,
	VkFence                                     fence)
{
	SERIALISE_ELEMENT(ResourceId, qid, GetResID(queue));
	SERIALISE_ELEMENT(ResourceId, imid, GetResID(image));

	SERIALISE_ELEMENT(uint32_t, num, numBindings);
	SERIALISE_ELEMENT_ARR(VkSparseImageMemoryBindInfo, binds, pBindInfo, num);
	
	if(m_State < WRITING && GetResourceManager()->HasLiveResource(imid))
	{
		queue = GetResourceManager()->GetLiveHandle<VkQueue>(qid);
		image = GetResourceManager()->GetLiveHandle<VkImage>(imid);

		ObjDisp(queue)->QueueBindSparseImageMemory(Unwrap(queue), Unwrap(image), num, binds);
	}

	SAFE_DELETE_ARRAY(binds);

	return true;
}

VkResult WrappedVulkan::vkQueueBindSparse(
	VkQueue                                     queue,
	uint32_t                                    bindInfoCount,
	const VkBindSparseInfo*                     pBindInfo,
	VkFence                                     fence)
{
	if(m_State >= WRITING_CAPFRAME)
	{
		CACHE_THREAD_SERIALISER();
		
		SCOPED_SERIALISE_CONTEXT(BIND_SPARSE);
		Serialise_vkQueueBindSparse(localSerialiser, queue, bindInfoCount, pBindInfo, fence);

		m_FrameCaptureRecord->AddChunk(scope.Get());
		GetResourceManager()->MarkResourceFrameReferenced(GetResID(queue), eFrameRef_Read);
		GetResourceManager()->MarkResourceFrameReferenced(GetResID(fence), eFrameRef_Read);
		// images/buffers aren't marked referenced. If the only ref is a memory bind, we just skip it
	}

	if(m_State >= WRITING)
	{
		GetRecord(image)->sparseInfo->Update(numBindings, pBindInfo);
	}
	
	VkSparseImageMemoryBindInfo *unwrappedBinds = GetTempArray<VkSparseImageMemoryBindInfo>(numBindings);
	memcpy(unwrappedBinds, pBindInfo, sizeof(VkSparseImageMemoryBindInfo)*numBindings);
	for(uint32_t i=0; i < numBindings; i++) unwrappedBinds[i].mem = Unwrap(unwrappedBinds[i].mem);

	return ObjDisp(queue)->QueueBindSparseImageMemory(Unwrap(queue), Unwrap(image), numBindings, unwrappedBinds);
}
#endif

bool WrappedVulkan::Serialise_vkQueueWaitIdle(Serialiser* localSerialiser, VkQueue queue)
{
	SERIALISE_ELEMENT(ResourceId, id, GetResID(queue));
	
	if(m_State < WRITING)
	{
		queue = GetResourceManager()->GetLiveHandle<VkQueue>(id);
		ObjDisp(queue)->QueueWaitIdle(Unwrap(queue));
	}

	return true;
}

VkResult WrappedVulkan::vkQueueWaitIdle(VkQueue queue)
{
	VkResult ret = ObjDisp(queue)->QueueWaitIdle(Unwrap(queue));
	
	if(m_State >= WRITING_CAPFRAME)
	{
		CACHE_THREAD_SERIALISER();
		
		SCOPED_SERIALISE_CONTEXT(QUEUE_WAIT_IDLE);
		Serialise_vkQueueWaitIdle(localSerialiser, queue);

		m_FrameCaptureRecord->AddChunk(scope.Get());
		GetResourceManager()->MarkResourceFrameReferenced(GetResID(queue), eFrameRef_Read);
	}

	return ret;
}
