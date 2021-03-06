//
// Copyright © 2017 Arm Ltd. All rights reserved.
// SPDX-License-Identifier: MIT
//

#include "ProfilingService.hpp"

#include <boost/log/trivial.hpp>
#include <boost/format.hpp>

namespace armnn
{

namespace profiling
{

void ProfilingService::ResetExternalProfilingOptions(const ExternalProfilingOptions& options,
                                                     bool resetProfilingService)
{
    // Update the profiling options
    m_Options = options;

    // Check if the profiling service needs to be reset
    if (resetProfilingService)
    {
        // Reset the profiling service
        Reset();
    }
}

ProfilingState ProfilingService::ConfigureProfilingService(
        const ExternalProfilingOptions& options,
        bool resetProfilingService)
{
    ResetExternalProfilingOptions(options, resetProfilingService);
    ProfilingState currentState = m_StateMachine.GetCurrentState();
    if (options.m_EnableProfiling)
    {
        switch (currentState)
        {
            case ProfilingState::Uninitialised:
                Update(); // should transition to NotConnected
                Update(); // will either stay in NotConnected because there is no server
                          // or will enter WaitingForAck.
                currentState = m_StateMachine.GetCurrentState();
                if (currentState == ProfilingState::WaitingForAck)
                {
                    Update(); // poke it again to send out the metadata packet
                }
                currentState = m_StateMachine.GetCurrentState();
                return currentState;
            case ProfilingState::NotConnected:
                Update(); // will either stay in NotConnected because there is no server
                          // or will enter WaitingForAck
                currentState = m_StateMachine.GetCurrentState();
                if (currentState == ProfilingState::WaitingForAck)
                {
                    Update(); // poke it again to send out the metadata packet
                }
                currentState = m_StateMachine.GetCurrentState();
                return currentState;
            default:
                return currentState;
        }
    }
    else
    {
        // Make sure profiling is shutdown
        switch (currentState)
        {
            case ProfilingState::Uninitialised:
            case ProfilingState::NotConnected:
                return currentState;
            default:
                Stop();
                return m_StateMachine.GetCurrentState();
        }
    }
}

void ProfilingService::Update()
{
    if (!m_Options.m_EnableProfiling)
    {
        // Don't run if profiling is disabled
        return;
    }

    ProfilingState currentState = m_StateMachine.GetCurrentState();
    switch (currentState)
    {
    case ProfilingState::Uninitialised:
        // Initialize the profiling service
        Initialize();

        // Move to the next state
        m_StateMachine.TransitionToState(ProfilingState::NotConnected);
        break;
    case ProfilingState::NotConnected:
        // Stop the command thread (if running)
        m_CommandHandler.Stop();

        // Stop the send thread (if running)
        m_SendCounterPacket.Stop(false);

        // Stop the periodic counter capture thread (if running)
        m_PeriodicCounterCapture.Stop();

        // Reset any existing profiling connection
        m_ProfilingConnection.reset();

        try
        {
            // Setup the profiling connection
            BOOST_ASSERT(m_ProfilingConnectionFactory);
            m_ProfilingConnection = m_ProfilingConnectionFactory->GetProfilingConnection(m_Options);
        }
        catch (const Exception& e)
        {
            BOOST_LOG_TRIVIAL(warning) << "An error has occurred when creating the profiling connection: "
                                       << e.what() << std::endl;
        }

        // Move to the next state
        m_StateMachine.TransitionToState(m_ProfilingConnection
                                         ? ProfilingState::WaitingForAck  // Profiling connection obtained, wait for ack
                                         : ProfilingState::NotConnected); // Profiling connection failed, stay in the
                                                                          // "NotConnected" state
        break;
    case ProfilingState::WaitingForAck:
        BOOST_ASSERT(m_ProfilingConnection);

        // Start the command thread
        m_CommandHandler.Start(*m_ProfilingConnection);

        // Start the send thread, while in "WaitingForAck" state it'll send out a "Stream MetaData" packet waiting for
        // a valid "Connection Acknowledged" packet confirming the connection
        m_SendCounterPacket.Start(*m_ProfilingConnection);

        // The connection acknowledged command handler will automatically transition the state to "Active" once a
        // valid "Connection Acknowledged" packet has been received

        break;
    case ProfilingState::Active:

        // The period counter capture thread is started by the Periodic Counter Selection command handler upon
        // request by an external profiling service

        break;
    default:
        throw RuntimeException(boost::str(boost::format("Unknown profiling service state: %1")
                                          % static_cast<int>(currentState)));
    }
}

void ProfilingService::Disconnect()
{
    ProfilingState currentState = m_StateMachine.GetCurrentState();
    switch (currentState)
    {
    case ProfilingState::Uninitialised:
    case ProfilingState::NotConnected:
    case ProfilingState::WaitingForAck:
        return; // NOP
    case ProfilingState::Active:
        // Stop the command thread (if running)
        Stop();

        break;
    default:
        throw RuntimeException(boost::str(boost::format("Unknown profiling service state: %1")
                                          % static_cast<int>(currentState)));
    }
}

const ICounterDirectory& ProfilingService::GetCounterDirectory() const
{
    return m_CounterDirectory;
}

ProfilingState ProfilingService::GetCurrentState() const
{
    return m_StateMachine.GetCurrentState();
}

uint16_t ProfilingService::GetCounterCount() const
{
    return m_CounterDirectory.GetCounterCount();
}

bool ProfilingService::IsCounterRegistered(uint16_t counterUid) const
{
    return counterUid < m_CounterIndex.size();
}

uint32_t ProfilingService::GetCounterValue(uint16_t counterUid) const
{
    CheckCounterUid(counterUid);
    std::atomic<uint32_t>* counterValuePtr = m_CounterIndex.at(counterUid);
    BOOST_ASSERT(counterValuePtr);
    return counterValuePtr->load(std::memory_order::memory_order_relaxed);
}

void ProfilingService::SetCounterValue(uint16_t counterUid, uint32_t value)
{
    CheckCounterUid(counterUid);
    std::atomic<uint32_t>* counterValuePtr = m_CounterIndex.at(counterUid);
    BOOST_ASSERT(counterValuePtr);
    counterValuePtr->store(value, std::memory_order::memory_order_relaxed);
}

uint32_t ProfilingService::AddCounterValue(uint16_t counterUid, uint32_t value)
{
    CheckCounterUid(counterUid);
    std::atomic<uint32_t>* counterValuePtr = m_CounterIndex.at(counterUid);
    BOOST_ASSERT(counterValuePtr);
    return counterValuePtr->fetch_add(value, std::memory_order::memory_order_relaxed);
}

uint32_t ProfilingService::SubtractCounterValue(uint16_t counterUid, uint32_t value)
{
    CheckCounterUid(counterUid);
    std::atomic<uint32_t>* counterValuePtr = m_CounterIndex.at(counterUid);
    BOOST_ASSERT(counterValuePtr);
    return counterValuePtr->fetch_sub(value, std::memory_order::memory_order_relaxed);
}

uint32_t ProfilingService::IncrementCounterValue(uint16_t counterUid)
{
    CheckCounterUid(counterUid);
    std::atomic<uint32_t>* counterValuePtr = m_CounterIndex.at(counterUid);
    BOOST_ASSERT(counterValuePtr);
    return counterValuePtr->operator++(std::memory_order::memory_order_relaxed);
}

uint32_t ProfilingService::DecrementCounterValue(uint16_t counterUid)
{
    CheckCounterUid(counterUid);
    std::atomic<uint32_t>* counterValuePtr = m_CounterIndex.at(counterUid);
    BOOST_ASSERT(counterValuePtr);
    return counterValuePtr->operator--(std::memory_order::memory_order_relaxed);
}

ProfilingDynamicGuid ProfilingService::NextGuid()
{
    return m_GuidGenerator.NextGuid();
}

ProfilingStaticGuid ProfilingService::GenerateStaticId(const std::string& str)
{
    return m_GuidGenerator.GenerateStaticId(str);
}

std::unique_ptr<ISendTimelinePacket> ProfilingService::GetSendTimelinePacket() const
{
    return m_TimelinePacketWriterFactory.GetSendTimelinePacket();
}

void ProfilingService::Initialize()
{
    // Register a category for the basic runtime counters
    if (!m_CounterDirectory.IsCategoryRegistered("ArmNN_Runtime"))
    {
        m_CounterDirectory.RegisterCategory("ArmNN_Runtime");
    }

    // Register a counter for the number of loaded networks
    if (!m_CounterDirectory.IsCounterRegistered("Loaded networks"))
    {
        const Counter* loadedNetworksCounter =
                m_CounterDirectory.RegisterCounter("ArmNN_Runtime",
                                                   0,
                                                   0,
                                                   1.f,
                                                   "Loaded networks",
                                                   "The number of networks loaded at runtime",
                                                   std::string("networks"));
        BOOST_ASSERT(loadedNetworksCounter);
        InitializeCounterValue(loadedNetworksCounter->m_Uid);
    }

    // Register a counter for the number of registered backends
    if (!m_CounterDirectory.IsCounterRegistered("Registered backends"))
    {
        const Counter* registeredBackendsCounter =
                m_CounterDirectory.RegisterCounter("ArmNN_Runtime",
                                                   0,
                                                   0,
                                                   1.f,
                                                   "Registered backends",
                                                   "The number of registered backends",
                                                   std::string("backends"));
        BOOST_ASSERT(registeredBackendsCounter);
        InitializeCounterValue(registeredBackendsCounter->m_Uid);
    }

    // Register a counter for the number of inferences run
    if (!m_CounterDirectory.IsCounterRegistered("Inferences run"))
    {
        const Counter* inferencesRunCounter =
                m_CounterDirectory.RegisterCounter("ArmNN_Runtime",
                                                   0,
                                                   0,
                                                   1.f,
                                                   "Inferences run",
                                                   "The number of inferences run",
                                                   std::string("inferences"));
        BOOST_ASSERT(inferencesRunCounter);
        InitializeCounterValue(inferencesRunCounter->m_Uid);
    }
}

void ProfilingService::InitializeCounterValue(uint16_t counterUid)
{
    // Increase the size of the counter index if necessary
    if (counterUid >= m_CounterIndex.size())
    {
        m_CounterIndex.resize(boost::numeric_cast<size_t>(counterUid) + 1);
    }

    // Create a new atomic counter and add it to the list
    m_CounterValues.emplace_back(0);

    // Register the new counter to the counter index for quick access
    std::atomic<uint32_t>* counterValuePtr = &(m_CounterValues.back());
    m_CounterIndex.at(counterUid) = counterValuePtr;
}

void ProfilingService::Reset()
{
    // Stop the profiling service...
    Stop();

    // ...then delete all the counter data and configuration...
    m_CounterIndex.clear();
    m_CounterValues.clear();
    m_CounterDirectory.Clear();

    // ...finally reset the profiling state machine
    m_StateMachine.Reset();
}

void ProfilingService::Stop()
{
    // The order in which we reset/stop the components is not trivial!

    // First stop the threads (Command Handler first)...
    m_CommandHandler.Stop();
    m_SendCounterPacket.Stop(false);
    m_PeriodicCounterCapture.Stop();

    // ...then close and destroy the profiling connection...
    if (m_ProfilingConnection != nullptr && m_ProfilingConnection->IsOpen())
    {
        m_ProfilingConnection->Close();
    }
    m_ProfilingConnection.reset();

    // ...then move to the "NotConnected" state
    m_StateMachine.TransitionToState(ProfilingState::NotConnected);
}

inline void ProfilingService::CheckCounterUid(uint16_t counterUid) const
{
    if (!IsCounterRegistered(counterUid))
    {
        throw InvalidArgumentException(boost::str(boost::format("Counter UID %1% is not registered") % counterUid));
    }
}

} // namespace profiling

} // namespace armnn
