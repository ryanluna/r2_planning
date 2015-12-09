/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2015, Rice University
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the Rice University nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/

/* Author: Ryan Luna */

#include <queue>
#include "ompl/geometric/planners/hilo/XXL.h"
#include "ompl/base/goals/GoalSampleableRegion.h"
#include "ompl/base/goals/GoalLazySamples.h"
#include "ompl/tools/config/MagicConstants.h"
#include "ompl/tools/config/SelfConfig.h"
#include "ompl/util/Exception.h"

ompl::geometric::XXL::XXL(const ompl::base::SpaceInformationPtr &si) : base::Planner(si, "XXL"), topLayer_(NULL), kill_(false)
{

}

ompl::geometric::XXL::XXL(const ompl::base::SpaceInformationPtr &si, const XXLDecompositionPtr& decomp) : base::Planner(si, "XXL"), topLayer_(NULL), kill_(false)
{
    setDecomposition(decomp);
}

ompl::geometric::XXL::~XXL()
{
    freeMemory();
}

void ompl::geometric::XXL::clear()
{
    Planner::clear();
    freeMemory();

    if (decomposition_)
    {
        // Layer storage
        // TODO: Implement a clear function to avoid memory reallocation
        if (topLayer_)
            delete topLayer_;
        topLayer_ = new Layer(-1, decomposition_->getNumRegions(), 0, NULL);  // top layer get -1 as id and a NULL parent
        allocateLayers(topLayer_);
    }

    motions_.clear();
    startMotions_.clear();
    goalMotions_.clear();
    goalCount_.clear();

    realGraph_.clear();
    lazyGraph_.clear();

    statesConnectedInRealGraph_ = 0;
    kill_ = false;
}

void ompl::geometric::XXL::freeMemory()
{
    for(size_t i = 0; i < motions_.size(); ++i)
    {
        si_->freeState(motions_[i]->state);
        delete motions_[i];
    }
    motions_.clear();

    if (topLayer_)
    {
        delete topLayer_;
        topLayer_ = NULL;
    }
}

void ompl::geometric::XXL::setup()
{
    if (!decomposition_)
    {
        OMPL_ERROR("%s: Decomposition is not set.  Cannot continue setup.", getName().c_str());
        throw;
    }

    Planner::setup();

    sampler_ = si_->allocStateSampler();

    statesConnectedInRealGraph_ = 0;

    maxGoalStatesPerRegion_ = 25;  // todo: make this a parameter
    maxGoalStates_ = 500;  // todo: Make this a parameter

    kill_ = false;

    // Layer storage - Allocating new storage
    if (topLayer_)
        delete topLayer_;
    topLayer_ = new Layer(-1, decomposition_->getNumRegions(), 0, NULL);
    allocateLayers(topLayer_);
}

void ompl::geometric::XXL::setDecomposition(const XXLDecompositionPtr& decomp)
{
    decomposition_ = decomp;
    predecessors_.resize(decomposition_->getNumRegions());
    closedList_.resize(decomposition_->getNumRegions());

    if (decomposition_->numLayers() < 1)
        throw ompl::Exception("Decomposition must have at least one layer of projection");
}

// TODO: Do this dynamically.  In other words, only allocate a layer/region when we use it
void ompl::geometric::XXL::allocateLayers(Layer* layer)
{
    if (!layer)
        return;

    if (layer->getLevel() < decomposition_->numLayers()-1)
    {
        layer->allocateSublayers();
        if (layer->getLevel()+1 < decomposition_->numLayers()-1)
            for(size_t i = 0; i < layer->numRegions(); ++i)
                allocateLayers(layer->getSublayer(i));
    }
}

void ompl::geometric::XXL::updateRegionConnectivity(const Motion* m1, const Motion* m2, int layer)
{
    if (layer >= decomposition_->numLayers() || layer < 0) // recursive stop
        return;

    const ompl::base::StateSpacePtr& ss = si_->getStateSpace();
    base::State* xstate = si_->allocState();

    // check the projections of m1 and m2 in all layers at or below layer.  If any one level isn't adjacent, we need to add states
    std::vector<base::State*> intermediateStates;
    std::vector<std::vector<int> > intProjections;
    double dt = 0.05;
    for(int i = layer; i < decomposition_->numLayers(); ++i)
    {
        if (m1->levels[i] != m2->levels[i])
        {
            std::vector<int> nbrs;
            decomposition_->getNeighborhood(m1->levels[i], nbrs);
            bool isNeighbor = false;
            for(size_t j = 0; j < nbrs.size() && !isNeighbor; ++j)
                isNeighbor = nbrs[j] == m2->levels[i];

            if (!isNeighbor) // interpolate states between the motions
            {
                double t = dt;
                while (t < (1.0 - dt/2.0))
                {
                    std::vector<int> projection;
                    ss->interpolate(m1->state, m2->state, t, xstate);
                    decomposition_->project(xstate, projection);

                    intermediateStates.push_back(si_->cloneState(xstate));
                    intProjections.push_back(projection);

                    t += dt;
                }
                break;
            }
        }
    }

    if (intermediateStates.size())
    {
        std::vector<int> gaps;

        // see if we still need to fill any gaps
        for(size_t i = 1; i < intProjections.size(); ++i) // state
        {
            for(int j = layer; j < (int)intProjections[i].size(); ++j)  // layer
            {
                if (intProjections[i-1][j] != intProjections[i][j])
                {
                    std::vector<int> nbrs;
                    decomposition_->getNeighborhood(intProjections[i-1][j], nbrs);
                    bool isNeighbor = false;
                    for(size_t k = 0; k < nbrs.size() && !isNeighbor; ++k)
                        isNeighbor = nbrs[k] == intProjections[i][j];

                    if (!isNeighbor)
                    {
                        gaps.push_back((int)i);
                        break;
                    }
                }
            }
        }

        std::vector<std::vector<base::State*> > gapStates;
        std::vector<std::vector<std::vector<int> > > gapProjections;

        for(size_t i = 0; i < gaps.size(); ++i)
        {
            std::vector<base::State*> gapState;
            std::vector<std::vector<int> > gapProj;

            double t = (gaps[i]) * dt;
            double end = t + dt;
            double newdt = dt * dt;
            while (t < end)
            {
                std::vector<int> projection;
                ss->interpolate(m1->state, m2->state, t, xstate);
                decomposition_->project(xstate, projection);

                gapState.push_back(si_->cloneState(xstate));
                gapProj.push_back(projection);
                t += newdt;
            }

            gapStates.push_back(gapState);
            gapProjections.push_back(gapProj);
        }

        // add the gaps
        int offset = 0;
        for(size_t i = 0; i < gaps.size(); ++i)
        {
            std::vector<base::State*>::iterator start = intermediateStates.begin() + gaps[i] + offset;  // insert before this position
            intermediateStates.insert(start, gapStates[i].begin(), gapStates[i].end());

            std::vector<std::vector<int> >::iterator startproj = intProjections.begin() + gaps[i] + offset;
            intProjections.insert(startproj, gapProjections[i].begin(), gapProjections[i].end());

            offset += gapStates[i].size();
        }
    }

    // Now, add the states where we jump regions
    const Motion* prev = m1;
    for(size_t i = 0; i < intermediateStates.size(); ++i)
    {
        for(int j = layer; j < decomposition_->numLayers(); ++j)
        {
            if (prev->levels[j] != intProjections[i][j])
            {
                // add the new state
                int id = addState(intermediateStates[i]);
                const Motion* newMotion = motions_[id];

                // This state is placed in the real graph too
                Layer* l = topLayer_;
                for(size_t k = 0; k < newMotion->levels.size(); ++k)
                {
                    l->getRegion(newMotion->levels[k]).motionsInTree.push_back(newMotion->index);
                    if (l->hasSublayers())
                        l = l->getSublayer(newMotion->levels[k]);
                }

                // Connect the new state to prev
                lazyGraph_.removeEdge(prev->index, newMotion->index);  // remove the edge so we never try this again
                double weight = si_->distance(prev->state, newMotion->state);
                realGraph_.addEdge(prev->index, newMotion->index, weight);
                if (realGraph_.numNeighbors(prev->index) == 1)
                    statesConnectedInRealGraph_++;
                if (realGraph_.numNeighbors(newMotion->index) == 1)
                    statesConnectedInRealGraph_++;

                Layer* l1 = getLayer(prev->levels, j);
                Layer* l2 = getLayer(newMotion->levels, j);

                l1->getRegionGraph().addEdge(prev->levels[j], newMotion->levels[j]);
                if (l1 != l2)
                    l2->getRegionGraph().addEdge(prev->levels[j], newMotion->levels[j]);

                prev = newMotion;
            }
        }
    }

    si_->freeState(xstate);
}

ompl::geometric::XXL::Layer* ompl::geometric::XXL::getLayer(const std::vector<int>& regions, int layer)
{
    if (layer >= decomposition_->numLayers())
        throw ompl::Exception("Requested invalid layer");

    Layer* l = topLayer_;
    for(int i = 0; i < layer; ++i)
        l = l->getSublayer(regions[i]);
    return l;
}

int ompl::geometric::XXL::addThisState(base::State* state)
{
    // Could have lots of threads in here.  Let's keep it clean
    //boost::mutex::scoped_lock lock(stateMutex_);
    //stateMutex_.lock();

    Motion* motion = new Motion();
    motion->state = state;
    decomposition_->project(motion->state, motion->levels);

    // Adding state to both lazy graph and the real graph
    motion->index = realGraph_.addVertex();
    if (lazyGraph_.addVertex() != motion->index)
        throw;

    // Adding lazy edges to all states in the neighborhood
    std::vector<int> nbrs;
    decomposition_->getNeighbors(motion->levels[0], nbrs);
    nbrs.push_back(motion->levels[0]); // add this region
    for(size_t i = 0; i < nbrs.size(); ++i)
    {
        const std::vector<int>& nbrMotions = topLayer_->getRegion(nbrs[i]).allMotions;
        for(size_t j = 0; j < nbrMotions.size(); ++j)
        {
            const Motion* nbrMotion = motions_[nbrMotions[j]];
            double weight = si_->distance(motion->state, nbrMotion->state);
            bool added = lazyGraph_.addEdge(motion->index, nbrMotion->index, weight);
            if (!added)
                throw std::runtime_error("Failed to add edge to graph");
        }
    }

    // Inserting motion into the list
    motions_.push_back(motion);
    assert(motion->index == motions_.size()-1);

    // Updating sublayer storage with this motion
    Layer* layer = topLayer_;
    for(size_t i = 0; i < motion->levels.size(); ++i)
    {
        layer->getRegion(motion->levels[i]).allMotions.push_back(motion->index);
        if (layer->hasSublayers())
            layer = layer->getSublayer(motion->levels[i]);
        else if (i != motion->levels.size()-1)
            throw;
    }

    //stateMutex_.unlock();
    return motion->index;
}

int ompl::geometric::XXL::addState(const base::State* state)
{
    return addThisState(si_->cloneState(state));
}

int ompl::geometric::XXL::addGoalState(const base::State* state)
{
    std::vector<int> proj;
    decomposition_->project(state, proj);
    boost::unordered_map<std::vector<int>, int>::iterator it = goalCount_.find(proj);
    if (it != goalCount_.end() && it->second > (int)maxGoalStatesPerRegion_)
        return -1;
    else
    {
        if (it != goalCount_.end())
            goalCount_[proj]++;
        else
            goalCount_[proj] = 1;
    }


    int id = addState(state);  // adds state to the graph (no neighbors) and updates statistics
    assert(id >= 0);

    const Motion* motion = motions_[id];
    goalMotions_.push_back(motion->index);

    // Goal states are always placed in the real graph
    // Updating sublayer storage with this motion
    Layer* layer = topLayer_;
    for(size_t i = 0; i < motion->levels.size(); ++i)
    {
        layer->addGoalState(motion->levels[i], id);

        layer->getRegion(motion->levels[i]).motionsInTree.push_back(motion->index);
        if (layer->hasSublayers())
            layer = layer->getSublayer(motion->levels[i]);
        else if (i != motion->levels.size()-1)
            throw;
    }

    // Try to join goal states together (goal roadmap)
    /*for(size_t i = 0; i < goalMotions_.size() - 1; ++i)
    {
        const Motion* other = motions_[goalMotions_[i]];

        if (lazyGraph_.edgeExists(motion->index, other->index) && si_->checkMotion(motion->state, other->state))
        {
            // remove this edge from lazy graph and add to real graph
            //double weight = lazyGraph_.getEdgeWeight(motion->index, other->index);  // bug here.  Edge exists, but weight does not.  Probably a bug in AdjacencyList after edge removal.
            double weight = si_->distance(motion->state, other->state);

            lazyGraph_.removeEdge(motion->index, other->index);
            realGraph_.addEdge(motion->index, other->index, weight);

            if(realGraph_.numNeighbors(motion->index) == 1)
                statesConnectedInRealGraph_++;
            if (realGraph_.numNeighbors(other->index) == 1)
                statesConnectedInRealGraph_++;

            // Connect these regions in the region graph
            updateRegionConnectivity(motion, other, 0);
        }
    }*/
    return motion->index;
}

int ompl::geometric::XXL::addStartState(const base::State* state)
{
    int id = addState(state);  // adds state to the graph (no neighbors) and updates statistics
    assert(id >= 0);

    const Motion* motion = motions_[id];
    startMotions_.push_back(motion->index);

    // Start states are always placed in the real graph
    // Updating sublayer storage with this motion
    Layer* layer = topLayer_;
    for(size_t i = 0; i < motion->levels.size(); ++i)
    {
        layer->getRegion(motion->levels[i]).motionsInTree.push_back(motion->index);
        if (layer->hasSublayers())
            layer = layer->getSublayer(motion->levels[i]);
    }

    return motion->index;
}

void ompl::geometric::XXL::updateRegionProperties(Layer* layer, int reg)
{
    // SIMPLIFYYYYY MAN!
    // Initialize all weights to 0.5
    // Nudge weights based on:
    // - States in region
    // - Edges in region
    // - Number of appearances in the lead

    const Region& region = layer->getRegion(reg);
    double& weight = layer->getWeights()[reg];


    int statesInRegion = (int)region.allMotions.size();
    int statesInLayer = (layer->getLevel() == 0 ? (int)motions_.size() : (int)layer->getParent()->getRegion(layer->getID()).allMotions.size());
    double stateRatio = statesInRegion / (double)statesInLayer;

    int connectedStatesInRegion = (int)region.motionsInTree.size();

    double connStateRatio = (statesInRegion > 0 ? connectedStatesInRegion / (double)statesInRegion : 0.0);
    double leadRatio = (layer->numLeads() ? layer->leadAppearances(reg) / (double)layer->numLeads() : 0.0);

    double newWeight = (exp(-stateRatio) * exp(-10.0 * connStateRatio)) +
                        (1.0 - exp(-leadRatio));

    // nudge the weight in the direction of new weight by the given factor
    double nudgeFactor = 0.1;
    weight += (newWeight - weight) * nudgeFactor;

    // clamp weight between 0 and 1
    weight = std::max(0.0, std::min(1.0, weight));
}

void ompl::geometric::XXL::updateRegionProperties(const std::vector<int>& regions)
{
    Layer* layer = getLayer(regions, 0);
    for(size_t i = 0; i < regions.size(); ++i)
    {
        updateRegionProperties(layer, regions[i]);

        if (layer->hasSublayers())
            layer = layer->getSublayer(regions[i]);
        else if (i != regions.size()-1)
            throw;
    }
}

void ompl::geometric::XXL::sampleStates(Layer* layer, const ompl::base::PlannerTerminationCondition &ptc)
{
    base::State* xstate = si_->allocState();
    std::vector<int> newStates;
    if (layer->getID() == -1) // top layer
    {
        // Just sample uniformly until ptc is triggered
        while(!ptc)
        {
            sampler_->sampleUniform(xstate);
            if (si_->isValid(xstate))
                newStates.push_back(addState(xstate));
        }
    }
    else
    {
        const std::vector<int>& states = layer->getParent()->getRegion(layer->getID()).allMotions;
        if (states.size() == 0)
            throw ompl::Exception("Cannot sample states in a subregion where there are no states");

        while (!ptc)
        {
            // pick a random state in the layer as the seed
            const Motion* seedMotion = motions_[states[rng_.uniformInt(0, states.size()-1)]];
            const base::State* seedState = seedMotion->state;
            // Returns a valid state
            if (decomposition_->sampleFromRegion(layer->getID(), xstate, seedState, layer->getLevel()-1))
            {
                int idx = addState(xstate);
                newStates.push_back(idx);
            }
        }
    }

    // update weights of the regions we added states to
    for(size_t i = 0; i < newStates.size(); ++i)
        updateRegionProperties(motions_[newStates[i]]->levels);

    si_->freeState(xstate);
}

void ompl::geometric::XXL::sampleAlongLead(Layer* layer, const std::vector<int>& lead, const ompl::base::PlannerTerminationCondition& ptc)
{
    base::State* xstate = si_->allocState();

    // try to put a valid state in every region, a chicken for every pot
    int numSampleAttempts = 10;
    std::vector<int> newStates;

    // If any region has relatively few states, sample more there
    int maxStateCount = 0;
    for(size_t i = 0; i < lead.size() && !ptc; ++i)
    {
        const Region& region = layer->getRegion(lead[i]);
        if ((int)region.allMotions.size() > maxStateCount)
            maxStateCount = region.allMotions.size();
    }

    for(size_t i = 0; i < lead.size() && !ptc; ++i)
    {
        const Region& region = layer->getRegion(lead[i]);
        double p = 1.0 - (region.allMotions.size() / (double)maxStateCount);
        if (rng_.uniform01() < p)
        {
            std::vector<int> nbrs;
            decomposition_->getNeighborhood(lead[i], nbrs);
            for(int j = 0; j < numSampleAttempts; ++j)
            {
                const ompl::base::State* seed = NULL;
                //if (layer->getLevel() > 0) // must find a seed.  Try states in the neighborhood.  There probably are some
                {
                    std::random_shuffle(nbrs.begin(), nbrs.end());
                    for(size_t k = 0; k < nbrs.size() && !seed; ++k)
                    {
                        const Region& nbrReg = layer->getRegion(nbrs[k]);
                        if (nbrReg.allMotions.size() > 0) // just pick any old state
                            seed = motions_[nbrReg.allMotions[rng_.uniformInt(0, nbrReg.allMotions.size()-1)]]->state;
                    }

                    if (!seed)
                        continue;
                }

                if (decomposition_->sampleFromRegion(lead[i], xstate, seed, layer->getLevel()))
                    newStates.push_back(addState(xstate));
            }
        }
    }

    // super special case
    if (lead.size() == 1 && !layer->hasSublayers())
    {
        std::vector<int> nbrs;
        decomposition_->getNeighborhood(lead[0], nbrs);
        nbrs.push_back(lead[0]);
        for(int j = 0; j < numSampleAttempts; ++j)
        {
            const ompl::base::State* seed = NULL;
            //if (layer->getLevel() > 0) // must find a seed.  Try states in the neighborhood.  There probably are some
            {
                std::random_shuffle(nbrs.begin(), nbrs.end());
                for(size_t k = 0; k < nbrs.size() && !seed; ++k)
                {
                    const Region& nbrReg = layer->getRegion(nbrs[k]);
                    if (nbrReg.allMotions.size() > 0) // just pick any old state
                        seed = motions_[nbrReg.allMotions[rng_.uniformInt(0, nbrReg.allMotions.size()-1)]]->state;
                }
                if (!seed)
                    continue;
            }

            if (decomposition_->sampleFromRegion(lead[0], xstate, seed, layer->getLevel()))
                newStates.push_back(addState(xstate));
        }
    }

    for(size_t i = 0; i < newStates.size(); ++i)
        updateRegionProperties(motions_[newStates[i]]->levels);
    for(size_t i = 0; i < lead.size(); ++i)
        updateRegionProperties(layer, lead[i]);

    si_->freeState(xstate);
}

int ompl::geometric::XXL::steerToRegion(Layer* layer, int from, int to)
{
    if (!decomposition_->canSteer())
        throw ompl::Exception("steerToRegion not implemented in decomposition");


    Region& fromRegion = layer->getRegion(from);
    //Region& toRegion   = layer->getRegion(to);

    if (fromRegion.motionsInTree.size() == 0)
    {
        OMPL_DEBUG("%s: %d -> %d, Layer %d", __FUNCTION__, from, to, layer->getLevel());
        throw ompl::Exception("Logic error: No existing states in the region to expand from");
    }

    // Select a motion at random in the from region
    int random = rng_.uniformInt(0, fromRegion.motionsInTree.size() - 1);
    const Motion* fromMotion = motions_[fromRegion.motionsInTree[random]];

    std::vector<base::State*> newStates;
    // Steer toward 'to' region.  If successful, a valid path is found between newStates
    if (decomposition_->steerToRegion(to, layer->getLevel(), fromMotion->state, newStates))
    {
        std::vector<int> newStateIDs(newStates.size());
        // add all states into the real graph
        for(size_t i = 0; i < newStates.size(); ++i)
            newStateIDs[i] = addThisState(newStates[i]);

        // Connect all states
        int prev = fromMotion->index;
        for(size_t i = 0; i < newStateIDs.size(); ++i)
        {
            // Add edge
            lazyGraph_.removeEdge(prev, newStateIDs[i]);
            double weight = si_->distance(motions_[prev]->state, motions_[newStateIDs[i]]->state);
            realGraph_.addEdge(prev, newStateIDs[i], weight);

            // Insert state into sublayers
            Layer* l = topLayer_;
            const Motion* newMotion = motions_[newStateIDs[i]];
            for(size_t j = 0; j < newMotion->levels.size(); ++j)
            {
                l->getRegion(newMotion->levels[j]).motionsInTree.push_back(newMotion->index);
                if (l->hasSublayers())
                    l = l->getSublayer(newMotion->levels[j]);
            }

            // Update states connected metric
            if (i == 0 && realGraph_.numNeighbors(prev) == 0)
                statesConnectedInRealGraph_++;
            if (realGraph_.numNeighbors(newStateIDs[i]) == 0)
                statesConnectedInRealGraph_++;

            // Update connectivity
            updateRegionConnectivity(motions_[prev], newMotion, layer->getLevel());
            updateRegionProperties(newMotion->levels);

            prev = newStateIDs[i];
        }

        return newStateIDs.back();
    }
    return -1;
}

// Expand the verified tree in region 'from' to region 'to' in the given layer
// Expansion is only from states connected to the start/goal in region 'from'
// A successful expansion will connect with an existing (connected) state in the region, or a newly sampled state
int ompl::geometric::XXL::expandToRegion(Layer* layer, int from, int to, bool useExisting)
{
    Region& fromRegion = layer->getRegion(from);
    Region& toRegion   = layer->getRegion(to);

    if (fromRegion.motionsInTree.size() == 0)
    {
        OMPL_DEBUG("%s: %d -> %d, Layer %d", __FUNCTION__, from, to, layer->getLevel());
        throw ompl::Exception("Logic error: No existing states in the region to expand from");
    }

    if (useExisting && toRegion.motionsInTree.size() == 0)
    {
        OMPL_ERROR("Logic error: useExisting is true but there are no existing states");
        return -1;
    }

    // Select a motion at random in the from region
    int random = rng_.uniformInt(0, fromRegion.motionsInTree.size() - 1);
    const Motion* fromMotion = motions_[fromRegion.motionsInTree[random]];

    // Select a motion in the 'to' region, or sample a new state
    const Motion* toMotion = NULL;
    if (useExisting || (toRegion.motionsInTree.size() > 0 && rng_.uniform01() < 0.50)) // use an existing state 50% of the time
    {
        for(size_t i = 0; i < toRegion.motionsInTree.size() && !toMotion; ++i)
            if (lazyGraph_.edgeExists(fromMotion->index, motions_[toRegion.motionsInTree[i]]->index))
                toMotion = motions_[toRegion.motionsInTree[i]];
    }

    bool newState = false;
    if (toMotion == NULL) // sample a new state
    {
        base::State* xstate = si_->allocState();
        if (decomposition_->sampleFromRegion(to, xstate, fromMotion->state, layer->getLevel()))
        {
            int id = addState(xstate);
            toMotion = motions_[id];
            newState = true;
        }
        si_->freeState(xstate);

        if (toMotion == NULL)
            return -1;
    }


    lazyGraph_.removeEdge(fromMotion->index, toMotion->index);  // remove the edge so we never try this again
    layer->selectRegion(to);

    // Try to connect the states
    if (si_->checkMotion(fromMotion->state, toMotion->state)) // Motion is valid!
    {
        // add edge to real graph
        double weight = si_->distance(fromMotion->state, toMotion->state);

        if (realGraph_.numNeighbors(fromMotion->index) == 0)
            statesConnectedInRealGraph_++;
        if (realGraph_.numNeighbors(toMotion->index) == 0)
            statesConnectedInRealGraph_++;
        realGraph_.addEdge(fromMotion->index, toMotion->index, weight);

        updateRegionConnectivity(fromMotion, toMotion, layer->getLevel());

        if (newState)
        {
            // Add this state to the real graph
            Layer* l = topLayer_;
            for(size_t i = 0; i < toMotion->levels.size(); ++i)
            {
                l->getRegion(toMotion->levels[i]).motionsInTree.push_back(toMotion->index);
                if (l->hasSublayers())
                    l = l->getSublayer(toMotion->levels[i]);
            }
        }
        return toMotion->index;
    }
    return -1;
}

// Check that each region in the lead has at least one state connected to the start and goal
bool ompl::geometric::XXL::feasibleLead(Layer* layer, const std::vector<int>& lead, const ompl::base::PlannerTerminationCondition& ptc)
{
    assert(lead.size() > 0);

    // Don't do anything with a one-region lead
    if (lead.size() == 1)
        return layer->getRegion(lead[0]).motionsInTree.size() > 0;

    // Figure out which regions we know how to get to (path in the tree)
    // There should always a known state at the beginning and end of the lead (start and goal)
    std::vector<bool> regionInTree(lead.size(), false);
    size_t numRegionsInLead = 0;
    for(size_t i = 0; i < lead.size(); ++i)
    {
        regionInTree[i] = layer->getRegion(lead[i]).motionsInTree.size() > 0;
        if (regionInTree[i])
            numRegionsInLead++;
    }

    // Connect unoccupied regions to an occupied neighbor in the lead
    // Attempt connection while we are making progress along the lead (forward and backward)
    bool expanded = true;
    int maxAttempts = 5;
    while(numRegionsInLead < lead.size() && expanded && !ptc)
    {
        expanded = false;
        for(size_t i = 1; i < lead.size() && !ptc; ++i)
        {
            int from = -1, to = -1;
            if (!regionInTree[i] && regionInTree[i-1]) // 'forward' along lead
            {
                from = i-1;
                to = i;
            }
            else if (regionInTree[i] && !regionInTree[i-1]) // 'backward along lead'
            {
                from = i;
                to = i-1;
            }

            if (from != -1)  // expand into new territory from existing state
            {
                //bool warned = false;
                for(int j = 0; j < maxAttempts && !ptc && !expanded; ++j)
                {
                    int idx;

                    if (decomposition_->canSteer())
                        idx = steerToRegion(layer, lead[from], lead[to]);
                    else
                        idx = expandToRegion(layer, lead[from], lead[to]);

                    // Success
                    if (idx != -1)
                    {
                        regionInTree[to] = true;
                        numRegionsInLead++;
                        expanded = true;
                    }
                }
            }
            else if (regionInTree[i] && regionInTree[i-1]) // bolster existing connection between these regions
            {
                const Region& r1 = layer->getRegion(lead[i-1]);
                const Region& r2 = layer->getRegion(lead[i]);
                double p1 = 1.0 - (r1.motionsInTree.size() / (double)r1.allMotions.size());
                double p2 = 1.0 - (r2.motionsInTree.size() / (double)r2.allMotions.size());
                double p = std::max(p1, p2);
                if(rng_.uniform01() < p) // improve existing connections
                    connectRegions(layer, lead[i-1], lead[i], ptc);
            }
        }
    }

    if (rng_.uniform01() < 0.05)  // small chance to brute force connect along lead
    {
        for(size_t i = 1; i < lead.size(); ++i)
            connectRegions(layer, lead[i-1], lead[i], ptc);
    }

    for(size_t i = 0; i < lead.size(); ++i)
        updateRegionProperties(layer, lead[i]);

    return numRegionsInLead == lead.size();
}

// We have a lead that has states in every region.  Try to connect the states together
bool ompl::geometric::XXL::connectLead(Layer* layer, const std::vector<int>& lead, std::vector<int>& candidateRegions, const ompl::base::PlannerTerminationCondition& ptc)
{
    if (lead.size() == 0)
        throw ompl::Exception("Lead is empty");

    AdjacencyList& regionGraph = layer->getRegionGraph();

    // Make sure there are connections between the regions along the lead.
    bool failed = false;
    if (lead.size() > 1)
    {
        bool connected = true;
        for(size_t i = 1; i < lead.size() && connected && !ptc; ++i)
        {
            bool regionsConnected = regionGraph.edgeExists(lead[i], lead[i-1]);

            const Region& r1 = layer->getRegion(lead[i-1]);
            const Region& r2 = layer->getRegion(lead[i]);
            double p1 = 1.0 - (r1.motionsInTree.size() / (double)r1.allMotions.size());
            double p2 = 1.0 - (r2.motionsInTree.size() / (double)r2.allMotions.size());
            double p = std::max(p1, p2);

            if (regionsConnected)
                p /= 2.0;

            if (!regionsConnected || rng_.uniform01() < p)
                connectRegions(layer, lead[i-1], lead[i], ptc);

            connected &= regionGraph.edgeExists(lead[i], lead[i-1]);
        }

        if (!connected)
            failed = true;
    }

    // Failed to connect all regions in the lead
    if (failed)
        return false;

    // Compute the candidate region connection point(s)
    std::set<int> startComponents;
    std::set<int> goalComponents;
    for(size_t i = 0; i < startMotions_.size(); ++i)
        startComponents.insert(realGraph_.getComponentID(startMotions_[i]));
    for(size_t i = 0; i < goalMotions_.size(); ++i)
        goalComponents.insert(realGraph_.getComponentID(goalMotions_[i]));

    // See if there are connected components that appear in every region in the lead
    // If so, there must be a solution path
    std::set<int> sharedComponents;
    for(std::set<int>::iterator it = startComponents.begin(); it != startComponents.end();)
    {
        std::set<int>::iterator goalIt = goalComponents.find(*it);
        if (goalIt != goalComponents.end())
        {
            sharedComponents.insert(*it);
            goalComponents.erase(goalIt);
            startComponents.erase(it++); // VERY important to increment iterator here.
        }
        else it++;
    }

    if (sharedComponents.size())  // there must be a path from start to goal
        return true;

    // Figure out which regions in the lead are connected to the start and/or goal
    std::vector<bool> inStartTree(lead.size(), false);
    std::vector<std::set<int> > validGoalComponents;
    for(size_t i = 0; i < lead.size(); ++i)
    {
        validGoalComponents.push_back(std::set<int>());

        const std::vector<int>& motions = layer->getRegion(lead[i]).motionsInTree;
        for(size_t j = 0; j < motions.size(); ++j)
        {
            int component = realGraph_.getComponentID(motions[j]);
            assert(component >= 0);

            if (startComponents.find(component) != startComponents.end())
                inStartTree[i] = true;
            else if (goalComponents.find(component) != goalComponents.end())
                validGoalComponents[i].insert(component);
        }
    }

    // intersect the valid goal components to find components that are in every region in the lead connected to the goal
    // start with the smallest non-empty set
    size_t min = validGoalComponents.size()-1;
    assert(validGoalComponents[min].size() > 0);
    for(size_t i = 0; i < validGoalComponents.size(); ++i)
        if (validGoalComponents[i].size() && validGoalComponents[i].size() < validGoalComponents[min].size())
            min = i;

    // The set of goal component ids that appear in all regions that are connected to a goal state
    std::set<int> leadGoalComponents(validGoalComponents[min].begin(), validGoalComponents[min].end());
    for(size_t i = 0; i < lead.size(); ++i)
    {
        if (i == min || validGoalComponents[i].size() == 0)
            continue;

        for(std::set<int>::iterator it = leadGoalComponents.begin(); it != leadGoalComponents.end();)
            if (validGoalComponents[i].find(*it) == validGoalComponents[i].end()) // candidate component not in the list
                leadGoalComponents.erase(it++);
            else it++;
    }

    // see if there is a region (or regions) that have states in both the start and the goal tree
    candidateRegions.clear();
    for(size_t i = 0; i < lead.size(); ++i)
    {
        if (inStartTree[i])
        {
            // check for goal tree - intersect validGoalComponents[i] and leadGoalComponents.  If non-empty, this is a winner
            for(std::set<int>::iterator it = validGoalComponents[i].begin(); it != validGoalComponents[i].end(); ++it)
                if (leadGoalComponents.find(*it) != leadGoalComponents.end())
                {
                    candidateRegions.push_back(lead[i]);
                    break;
                }
        }
    }

    // internal connection within a region
    for(size_t i = 0; i < candidateRegions.size(); ++i)
    {
        connectRegion(layer, candidateRegions[i], ptc);

        // See if there is a solution path
        for(size_t i = 0; i < startMotions_.size(); ++i)
            for(size_t j = 0; j < goalMotions_.size(); ++j)
                if (realGraph_.inSameComponent(startMotions_[i], goalMotions_[j]))  // yup, solution exists
                    return true;
    }

    if (candidateRegions.size() == 0)
    {
        // at this point we have not yet found a solution path.  see if there are other regions we can identify as candidates
        // if all regions are connected to the start and the end of the lead is connected to the goal, mark that as a candidate
        bool allConnectedToStart = true;
        for(size_t i = 0; i < inStartTree.size(); ++i)
            allConnectedToStart &= inStartTree[i];

        if (allConnectedToStart && validGoalComponents[lead.size()-1].size() > 0)
        {
            candidateRegions.push_back(lead.back());
            connectRegion(layer, lead.back(), ptc);

            // See if there is a solution path
            for(size_t i = 0; i < startMotions_.size(); ++i)
                for(size_t j = 0; j < goalMotions_.size(); ++j)
                    if (realGraph_.inSameComponent(startMotions_[i], goalMotions_[j]))  // yup, solution exists
                        return true;
        }
    }

    return false; // failed to find a solution path
}

// Try to connect nodes within a region that are not yet connected
void ompl::geometric::XXL::connectRegion(Layer* layer, int reg, const base::PlannerTerminationCondition &ptc)
{
    assert(layer);
    assert(reg >= 0 && reg < decomposition_->getNumRegions());

    Region& region = layer->getRegion(reg);
    const std::vector<int>& allMotions = region.allMotions;

    // Shuffle the motions in this regions
    std::vector<int> shuffledMotions(allMotions.begin(), allMotions.end());
    std::random_shuffle(shuffledMotions.begin(), shuffledMotions.end());

    size_t maxIdx = (shuffledMotions.size() > 20 ? shuffledMotions.size() / 2 : shuffledMotions.size());

    // This method will try to connect states in different connected components
    // of the real graph together
    #ifdef ENABLE_PROFILING
    ompl::luna::Profiler::Begin("connectRegion");
    #endif

    // Try to join disconnected states together within the region
    for(size_t i = 0; i < maxIdx && !ptc; ++i)
    {
        const Motion* m1 = motions_[shuffledMotions[i]];
        for(size_t j = i+1; j < maxIdx && !ptc; ++j)
        {
            const Motion* m2 = motions_[shuffledMotions[j]];
            if (lazyGraph_.edgeExists(m1->index, m2->index) && !realGraph_.inSameComponent(m1->index, m2->index))
            {
                // Remove this edge so we never try and verify this edge again
                lazyGraph_.removeEdge(m1->index, m2->index);

                // At this point, m1 and m2 should both be connected to start or goal, but
                // there is no path in the graph from m1 to m2.  Try to connect them together
                if (si_->checkMotion(m1->state, m2->state)) // Motion is valid!
                {
                    // add edge to real graph
                    double weight = si_->distance(m1->state, m2->state);
                    realGraph_.addEdge(m1->index, m2->index, weight);  // add state to real graph

                    // Update region storage
                    if (realGraph_.numNeighbors(m1->index) == 1 && !isStartState(m1->index) && !isGoalState(m1->index))
                    {
                        statesConnectedInRealGraph_++;

                        // Add this state to the real graph in all layers
                        Layer* l = topLayer_;
                        for(size_t i = 0; i < m1->levels.size(); ++i)
                        {
                            l->getRegion(m1->levels[i]).motionsInTree.push_back(m1->index);
                            if (l->hasSublayers())
                                l = l->getSublayer(m1->levels[i]);
                            else if (i != m1->levels.size()-1)
                                throw;
                        }
                    }
                    if (realGraph_.numNeighbors(m2->index) == 1 && !isStartState(m2->index) && !isGoalState(m2->index))
                    {
                        statesConnectedInRealGraph_++;

                        // Add this state to the real graph in all layers
                        Layer* l = topLayer_;
                        for(size_t i = 0; i < m2->levels.size(); ++i)
                        {
                            l->getRegion(m2->levels[i]).motionsInTree.push_back(m2->index);
                            if (l->hasSublayers())
                                l = l->getSublayer(m2->levels[i]);
                            else if (i != m2->levels.size()-1)
                                throw;
                        }
                    }

                    updateRegionConnectivity(m1, m2, layer->getLevel());

                    // They better be connected meow
                    assert(realGraph_.inSameComponent(m1->index, m2->index));
                }
            }
        }
    }
    #ifdef ENABLE_PROFILING
    ompl::luna::Profiler::End("connectRegion");
    #endif

    layer->connectRegion(reg);
    updateRegionProperties(layer, reg);
}

void ompl::geometric::XXL::connectRegions(Layer* layer, int r1, int r2, const base::PlannerTerminationCondition &ptc, bool all)
{
    assert(layer);
    assert(r1 >= 0 && r1 < decomposition_->getNumRegions());
    assert(r2 >= 0 && r2 < decomposition_->getNumRegions());

    //layer->connectRegion(r1);  // bad don't do this
    //layer->connectRegion(r2);  // bad don't do this

    // "select" the regions 20x
    layer->selectRegion(r1, 20);
    layer->selectRegion(r2, 20);

    Region& reg1 = layer->getRegion(r1);
    const std::vector<int>& allMotions1 = reg1.allMotions;
    // Shuffle the motions in r1
    std::vector<int> shuffledMotions1(allMotions1.begin(), allMotions1.end());
    std::random_shuffle(shuffledMotions1.begin(), shuffledMotions1.end());

    Region& reg2 = layer->getRegion(r2);
    const std::vector<int>& allMotions2 = reg2.allMotions;
    // Shuffle the motions in r2
    std::vector<int> shuffledMotions2(allMotions2.begin(), allMotions2.end());
    std::random_shuffle(shuffledMotions2.begin(), shuffledMotions2.end());

    size_t maxConnections = 25;
    size_t maxIdx1 = (all ? shuffledMotions1.size() : std::min(shuffledMotions1.size(), maxConnections));
    size_t maxIdx2 = (all ? shuffledMotions2.size() : std::min(shuffledMotions2.size(), maxConnections));

    #ifdef ENABLE_PROFILING
    ompl::luna::Profiler::Begin("connectRegions");
    #endif

    // Try to join different connected components within the region
    for(size_t i = 0; i < maxIdx1 && !ptc; ++i)
    {
        const Motion* m1 = motions_[shuffledMotions1[i]];
        for(size_t j = 0; j < maxIdx2 && !ptc; ++j)
        {
            const Motion* m2 = motions_[shuffledMotions2[j]];
            if (lazyGraph_.edgeExists(m1->index, m2->index) && !realGraph_.inSameComponent(m1->index, m2->index))
            {
                // Remove this edge so we never try and verify this edge again
                lazyGraph_.removeEdge(m1->index, m2->index);

                // At this point, m1 and m2 should both be connected to start or goal, but
                // there is no path in the graph from m1 to m2.  Try to connect them together
                if (si_->checkMotion(m1->state, m2->state)) // Motion is valid!
                {
                    // add edge to real graph
                    double weight = si_->distance(m1->state, m2->state);
                    realGraph_.addEdge(m1->index, m2->index, weight);  // add state to real graph

                    // Update region storage
                    if (realGraph_.numNeighbors(m1->index) == 1 && !isStartState(m1->index) && !isGoalState(m1->index))
                    {
                        statesConnectedInRealGraph_++;

                        // Add this state to the real graph in all layers
                        Layer* l = topLayer_;
                        for(size_t i = 0; i < m1->levels.size(); ++i)
                        {
                            l->getRegion(m1->levels[i]).motionsInTree.push_back(m1->index);
                            if (l->hasSublayers())
                                l = l->getSublayer(m1->levels[i]);
                        }
                    }
                    if (realGraph_.numNeighbors(m2->index) == 1 && !isStartState(m2->index) && !isGoalState(m2->index))
                    {
                        statesConnectedInRealGraph_++;

                        // Add this state to the real graph in all layers
                        Layer* l = topLayer_;
                        for(size_t i = 0; i < m2->levels.size(); ++i)
                        {
                            l->getRegion(m2->levels[i]).motionsInTree.push_back(m2->index);
                            if (l->hasSublayers())
                                l = l->getSublayer(m2->levels[i]);
                        }
                    }

                    updateRegionConnectivity(m1, m2, layer->getLevel());

                    // They better be connected meow
                    assert(realGraph_.inSameComponent(m1->index, m2->index));
                }
            }
        }
    }

    updateRegionProperties(layer, r1);
    updateRegionProperties(layer, r2);

    #ifdef ENABLE_PROFILING
    ompl::luna::Profiler::End("connectRegions");
    #endif
}

void ompl::geometric::XXL::computeLead(Layer* layer, std::vector<int>& lead)
{
    if (startMotions_.size() == 0)
        throw ompl::Exception("Cannot compute lead without at least one start state");
    //if (goalMotions_.size() == 0)
    //    throw ompl::Exception("Cannot compute lead without at least one goal state");

    int start, end;

    if (goalMotions_.size() == 0)
    {
        const Motion* s = motions_[startMotions_[rng_.uniformInt(0, startMotions_.size()-1)]];
        start = s->levels[layer->getLevel()];

        if (topLayer_->numRegions() == 1)
            end = 0;
        else
        {
            do
            {
                end = rng_.uniformInt(0, topLayer_->numRegions()-1);
            } while (start == end);
        }
    }
    else
    {
        const Motion* s = NULL;
        const Motion* e = NULL;

        if (layer->getLevel() == 0)
        {
            s = motions_[startMotions_[rng_.uniformInt(0, startMotions_.size()-1)]];
            e = motions_[goalMotions_[rng_.uniformInt(0, goalMotions_.size()-1)]];
        }
        else  // sublayers
        {
            std::set<int> startComponents;
            for(size_t i = 0; i < startMotions_.size(); ++i)
                startComponents.insert(realGraph_.getComponentID(startMotions_[i]));

            // pick a state at random that is connected to the start
            do
            {
                const Region& reg = layer->getRegion(rng_.uniformInt(0, layer->numRegions()-1));
                if (reg.motionsInTree.size())
                {
                    int random = rng_.uniformInt(0, reg.motionsInTree.size()-1);

                    int cid = realGraph_.getComponentID(reg.motionsInTree[random]);
                    for(std::set<int>::const_iterator it = startComponents.begin(); it != startComponents.end(); ++it)
                        if (cid == *it)
                        {
                            s = motions_[reg.motionsInTree[random]];
                            break;
                        }
                }
            } while (s == NULL);

            std::set<int> goalComponents;
            for(size_t i = 0; i < goalMotions_.size(); ++i)
                goalComponents.insert(realGraph_.getComponentID(goalMotions_[i]));

            // pick a state at random that is connected to the goal
            do
            {
                const Region& reg = layer->getRegion(rng_.uniformInt(0, layer->numRegions()-1));
                if (reg.motionsInTree.size())
                {
                    int random = rng_.uniformInt(0, reg.motionsInTree.size()-1);

                    int cid = realGraph_.getComponentID(reg.motionsInTree[random]);
                    for(std::set<int>::const_iterator it = goalComponents.begin(); it != goalComponents.end(); ++it)
                        if (cid == *it)
                        {
                            e = motions_[reg.motionsInTree[random]];
                            break;
                        }
                }
            } while (e == NULL);
        }

        start = s->levels[layer->getLevel()];
        end = e->levels[layer->getLevel()];
    }


    bool success = false;

    if (start == end)
    {
        lead.resize(1);
        lead[0] = start;
        success = true;
    }
    else
    {
        if(rng_.uniform01() < 0.95)
            success = shortestPath(start, end, lead, layer->getWeights()) && lead.size() > 0;
        else
            success = randomWalk(start, end, lead) && lead.size() > 0;
    }

    if (!success)
        throw ompl::Exception("Failed to compute lead", getName().c_str());
}

bool ompl::geometric::XXL::searchForPath(Layer* layer, const ompl::base::PlannerTerminationCondition& ptc)
{
    //if (goalMotions_.size() == 0) // cannot compute a lead without a goal state
    //    return false;

    // If there are promising subregions, pick one of them most of the time
    double p = layer->connectibleRegions() / ((double)layer->connectibleRegions() + 1);
    if (layer->hasSublayers() && layer->connectibleRegions() > 0 && rng_.uniform01() < p)
    {
        // TODO: Make this non-uniform?
        int subregion = layer->connectibleRegion(rng_.uniformInt(0, layer->connectibleRegions()-1));
        Layer* sublayer = layer->getSublayer(subregion);

        return searchForPath(sublayer, ptc);
    }
    else
    {
        //OMPL_DEBUG("%s: layer %d(%d)", __FUNCTION__, layer->getLevel(), layer->getID());

        std::vector<int> lead;
        computeLead(layer, lead);
        layer->markLead(lead);  // update weights along weight

        #ifdef DEBUG_PLANNER  // debugui
            std::ofstream fout("planner_debug.txt", std::ios_base::app);
            fout << "lead " << layer->getLevel() << " " << layer->getID() << " " << lead.size() << " ";
            for(size_t i = 0; i < lead.size(); ++i)
                fout << lead[i] << " ";
            fout << std::endl;
        #endif

        // sample states where they are needed along lead
        sampleAlongLead(layer, lead, ptc);

        // Every region in the lead has a valid state in it
        if (feasibleLead(layer, lead, ptc))
        {
            std::vector<int> candidates;
            // Find a feasible path through the lead
            if (connectLead(layer, lead, candidates, ptc))
            {
                constructSolutionPath();
                return true;
            }

            // No feasible path found, but see if we can descend in layers to focus
            /// the search in specific regions identified during connection attempts
            if (layer->hasSublayers())
            {
                // see if there are candidate regions for subexpansion
                for(size_t i = 0; i < candidates.size() && !ptc; ++i)
                {
                    Layer* sublayer = layer->getSublayer(candidates[i]);
                    if (searchForPath(sublayer, ptc))
                        return true;
                }
            }
        }
    }

    return false;
}

ompl::base::PlannerStatus ompl::geometric::XXL::solve(const ompl::base::PlannerTerminationCondition& ptc)
{
    if (!decomposition_)
        throw ompl::Exception("Decomposition is not set.  Cannot solve");

    checkValidity();

    // Making sure goal object is valid
    base::GoalSampleableRegion *gsr = dynamic_cast<base::GoalSampleableRegion*>(pdef_->getGoal().get());
    if (!gsr)
    {
        OMPL_ERROR("%s: Unknown type of goal", getName().c_str());
        return base::PlannerStatus::UNRECOGNIZED_GOAL_TYPE;
    }
    if (!gsr->couldSample())
    {
        OMPL_ERROR("%s: Insufficient states in sampleable goal region", getName().c_str());
        return base::PlannerStatus::INVALID_GOAL;
    }

    // Start goal sampling
    //kill_ = false;
    //goalStateThread_ = boost::thread(boost::bind(&ompl::geometric::XXL::getGoalStates, this, ptc));

    // Adding all start states
    while (const base::State *s = pis_.nextStart())
        addStartState(s);

    // There must be at least one valid start state
    if (startMotions_.size() == 0)
    {
        kill_ = true;
        //goalStateThread_.join();

        OMPL_ERROR("%s: No valid start states", getName().c_str());
        return base::PlannerStatus::INVALID_START;
    }

    OMPL_INFORM("%s: Operating over %d dimensional, %d layer decomposition with %d regions per layer", getName().c_str(), decomposition_->getDimension(), decomposition_->numLayers(), decomposition_->getNumRegions());

    bool foundSolution = false;
    #ifdef ENABLE_PROFILING
        ompl::luna::Profiler::Clear();
        ompl::luna::Profiler::Start();
    #endif

    while (!ptc && !foundSolution)
    {
        #ifdef ENABLE_PROFILING
        if (!ompl::luna::Profiler::Running())
            throw ompl::Exception("profiler not running");
        #endif

        // double deltaT = 0.1;
        // if (motions_.size() < 25000)  // capping this seems to help focus the search more
        // {
        //     ompl::luna::Profiler::Begin("sample");
        //     sampleStates(base::plannerOrTerminationCondition(ptc, base::timedPlannerTerminationCondition(deltaT)));
        //     ompl::luna::Profiler::End("sample");
        // }
        // // TODO: As search progresses, increase time for search and decrease time for sampling
        // //foundSolution = searchForPath(topLayer_, base::plannerOrTerminationCondition(ptc, base::timedPlannerTerminationCondition(5.0 * deltaT)));

        // ompl::luna::Profiler::Begin("searchForPath");
        // foundSolution = searchForPath(topLayer_, ptc);
        // ompl::luna::Profiler::End("searchForPath");

        getGoalStates();  // non-threaded version
        foundSolution = searchForPath(topLayer_, ptc);
    }

    // if (!foundSolution && constructSolutionPath())
    // {
    //     OMPL_ERROR("Tripped and fell over a solution path.");
    //     foundSolution = true;
    // }

    #ifdef ENABLE_PROFILING
        ompl::luna::Profiler::Stop();
        ompl::luna::Profiler::Status();
    #endif

    OMPL_INFORM("%s: Created %lu states (%lu start, %lu goal); %u are connected", getName().c_str(), motions_.size(), startMotions_.size(), goalMotions_.size(), statesConnectedInRealGraph_);

    #ifdef DEBUG_PLANNER
        writeDebugOutput();
    #endif

    // Stop goal sampling thread, if it is still running
    kill_ = true;
    //goalStateThread_.join();

    if (foundSolution)
        return ompl::base::PlannerStatus::EXACT_SOLUTION;
    else
        return ompl::base::PlannerStatus::TIMEOUT;
}

void ompl::geometric::XXL::writeDebugOutput() const
{
#ifdef DEBUG_PLANNER
    std::ofstream fout("planner_debug.txt", std::ios_base::app);

    if (!fout)
        throw ompl::Exception("Failed to open debug output");

    // Roadmap
    // All vertices
    unsigned int dim = si_->getStateSpace()->getDimension();
    for(size_t i = 0; i < motions_.size(); ++i)
    {
        bool isStart = false;
        bool isGoal = false;

        for(size_t j = 0; j < startMotions_.size(); ++j)
            if (startMotions_[j] == (int)i)
                isStart = true;

        for(size_t j = 0; j < goalMotions_.size(); ++j)
            if (goalMotions_[j] == (int)i)
                isGoal = true;

        // metadata
        fout << "vertex " << i << " ";
        if (isStart)
            fout << "start ";
        else if (isGoal)
            fout << "goal ";
        else fout << "normal ";
        for(size_t j = 0; j < motions_[i]->levels.size(); ++j)
            fout << motions_[i]->levels[j] << " ";

        const double* angles = motions_[i]->state->as<PlanarManipulatorStateSpace::StateType>()->values;
        for(unsigned int j = 0; j < dim; ++j)
            fout << angles[j] << " ";
        fout << std::endl;
    }

    // edges
    for(size_t i = 0; i < motions_.size(); ++i)
    {
        std::vector<int> nbrs;
        realGraph_.getNeighbors(i, nbrs);
        for(size_t j = 0; j < nbrs.size(); ++j)
            fout << "edge " << i << " " << nbrs[j] << std::endl;

        // nbrs.clear();
        // lazyGraph_.getNeighbors(i, nbrs);
        // for(size_t j = 0; j < nbrs.size(); ++j)
        //     fout << "lazyedge " << i << " " << nbrs[j] << std::endl;
    }

    // weights
    Layer* layer = topLayer_;
    const std::vector<double>& weights = layer->getWeights();
    fout << "weights " << layer->getLevel() << " " << layer->getID() << " ";
    for(size_t i = 0; i < weights.size(); ++i)
        fout << weights[i] << " ";
    fout << std::endl;

    // subregions of interest
    for(int i = 0; i < layer->connectibleRegions(); ++i)
    {
        Layer* sublayer = layer->getSublayer(layer->connectibleRegion(i));
        const std::vector<double>& subweights = sublayer->getWeights();
        fout << "weights " << sublayer->getLevel() << " " << sublayer->getID() << " ";
        for(size_t j = 0; j < subweights.size(); ++j)
            fout << subweights[j] << " ";
        fout << std::endl;
    }

    fout.close();
#endif
}

bool ompl::geometric::XXL::isStartState(int idx) const
{
    for(size_t i = 0; i < startMotions_.size(); ++i)
        if (idx == startMotions_[i])
            return true;
    return false;
}

bool ompl::geometric::XXL::isGoalState(int idx) const
{
    for(size_t i = 0; i < goalMotions_.size(); ++i)
        if (idx == goalMotions_[i])
            return true;
    return false;
}

bool ompl::geometric::XXL::constructSolutionPath()
{
    int start = startMotions_[0];
    std::vector<int> predecessors;
    std::vector<double> cost;
    realGraph_.dijkstra(start, predecessors, cost);

    int bestGoal = -1;
    double bestCost = std::numeric_limits<double>::max();

    for(size_t i = 0; i < goalMotions_.size(); ++i)
    {
        if (cost[goalMotions_[i]] < bestCost)
        {
            bestCost = cost[goalMotions_[i]];
            bestGoal = goalMotions_[i];
        }
    }

    if (bestGoal == -1)
    {
        OMPL_ERROR("%s: No solution path exists.  constructSolutionPath() should never have been called", getName().c_str());
        return false;
    }


    std::vector<Motion*> slnPath;
    int v = bestGoal;
    while(predecessors[v] != v)
    {
        slnPath.push_back(motions_[v]);
        v = predecessors[v];
    }
    slnPath.push_back(motions_[v]);  // start

    PathGeometric *path = new PathGeometric(si_);
    for (int i = slnPath.size() - 1 ; i >= 0 ; --i)
        path->append(slnPath[i]->state);
    pdef_->addSolutionPath(base::PathPtr(path), false, 0.0, getName());

    return true;
}

void ompl::geometric::XXL::getPlannerData(ompl::base::PlannerData& data) const
{
    // Adding all vertices
    for(size_t i = 0; i < motions_.size(); ++i)
    {
        bool isStart = false;
        bool isGoal = false;

        for(size_t j = 0; j < startMotions_.size(); ++j)
            if (startMotions_[j] == (int)i)
                isStart = true;

        for(size_t j = 0; j < goalMotions_.size(); ++j)
            if (goalMotions_[j] == (int)i)
                isGoal = true;

        if (!isStart && !isGoal)
            data.addVertex(base::PlannerDataVertex(motions_[i]->state));
        else if (isStart)
            data.addStartVertex(base::PlannerDataVertex(motions_[i]->state));
        else if (isGoal)
            data.addGoalVertex(base::PlannerDataVertex(motions_[i]->state));
    }

    // Adding all edges
    for(size_t i = 0; i < motions_.size(); ++i)
    {
        std::vector<std::pair<int, double> > nbrs;
        realGraph_.getNeighbors(i, nbrs);

        for(size_t j = 0; j < nbrs.size(); ++j)
            data.addEdge(i, nbrs[j].first, ompl::base::PlannerDataEdge(), ompl::base::Cost(nbrs[j].second));
    }
}

void ompl::geometric::XXL::getGoalStates()
{
    int newCount = 0;
    int maxCount = 10;
    while (pis_.haveMoreGoalStates() && newCount < maxCount /*&& goalMotions_.size() < maxGoalStates_*/)
    {
        const base::State* st = pis_.nextGoal();
        if (st == NULL)
            break;

        newCount++;

        double dist = std::numeric_limits<double>::infinity();  // min distance to another goal state
        for(size_t i = 0; i < goalMotions_.size(); ++i)
        {
            double d = si_->distance(motions_[goalMotions_[i]]->state, st);
            if (d < dist)
                dist = d;
        }

        // Keep goals diverse.  TODO: GoalLazySamples does something similar to this.
        if (dist > 0.5)
            addGoalState(st);
    }
}

void ompl::geometric::XXL::getGoalStates(const base::PlannerTerminationCondition &/*ptc*/)
{
    throw ompl::Exception("Not thread safe");
    /*base::Goal* goal = pdef_->getGoal().get();
    while(!ptc && !kill_ && goalMotions_.size() < maxGoalStates_)
    {
        if (!pis_.haveMoreGoalStates())
        {
            usleep(1000);
            continue;
        }

        const base::State* st = pis_.nextGoal(ptc);
        if (st == NULL)
            break;

        double dist = std::numeric_limits<double>::infinity();  // min distance to another goal state
        for(size_t i = 0; i < goalMotions_.size(); ++i)
        {
            double d = si_->distance(motions_[goalMotions_[i]]->state, st);
            if (d < dist)
                dist = d;
        }

        // Keep goals diverse.  TODO: GoalLazySamples does something similar to this.
        if (dist > 0.5)
            addGoalState(st);
    }

    if (goalMotions_.size() >= maxGoalStates_)
       OMPL_INFORM("%s: Reached max state count in goal sampling thread", getName().c_str());
    OMPL_INFORM("%s: Goal sampling thread ceased", getName().c_str());*/
}

void ompl::geometric::XXL::getNeighbors(int rid, const std::vector<double>& weights, std::vector<std::pair<int, double> >& neighbors) const
{
    std::vector<int> nbrs;
    decomposition_->getNeighbors(rid, nbrs);

    for(size_t i = 0; i < nbrs.size(); ++i)
    {
        neighbors.push_back(std::make_pair(nbrs[i], weights[nbrs[i]]));
    }
}

struct OpenListNode
{
    int id;
    int parent;
    double g, h;

    OpenListNode() : id(-1), parent(-1), g(0.0), h(std::numeric_limits<double>::infinity()) {}
    OpenListNode(int _id) : id(_id), parent(-1), g(0.0), h(std::numeric_limits<double>::infinity()) {}

    bool operator < (const OpenListNode& other) const
    {
        return (g + h) > (other.g + other.h); // priority queue is a max heap, but we want nodes with smaller g+h to have higher priority
    }
};

// (weighted) A* search
bool ompl::geometric::XXL::shortestPath(int r1, int r2, std::vector<int>& path, const std::vector<double>& weights)
{
    if (r1 < 0 || r1 >= decomposition_->getNumRegions())
    {
        OMPL_ERROR("Start region (%d) is not valid", r1);
        return false;
    }

    if (r2 < 0 || r2 >= decomposition_->getNumRegions())
    {
        OMPL_ERROR("Goal region (%d) is not valid", r2);
        return false;
    }

    // 50% of time, do weighted A* instead of normal A*
    double weight = 1.0;  // weight = 1; normal A*
    if (rng_.uniform01() < 0.50)
    {
        if (rng_.uniform01() < 0.50)
            weight = 0.01; // greedy search
        else
            weight = 50.0; // weighted A*
    }

    // Initialize predecessors and open list
    std::fill(predecessors_.begin(), predecessors_.end(), -1);
    std::fill(closedList_.begin(), closedList_.end(), false);

    // Create empty open list
    std::priority_queue<OpenListNode> openList;

    // Add start node to open list
    OpenListNode start(r1);
    start.g = 0.0;
    start.h = weight * decomposition_->distanceHeuristic(r1, r2);
    start.parent = r1; // start has a self-transition to parent
    openList.push(start);

    // A* search
    bool solution = false;
    while (!openList.empty())
    {
        OpenListNode node = openList.top();
        openList.pop();

        // been here before
        if (closedList_[node.id])
            continue;

        // mark node as 'been here'
        closedList_[node.id] = true;
        predecessors_[node.id] = node.parent;

        // found solution!
        if (node.id == r2)
        {
            solution = true;
            break;
        }

        // Go through neighbors and add them to open list, if necessary
        std::vector<std::pair<int, double> > neighbors;
        getNeighbors(node.id, weights, neighbors);

        // Shuffle neighbors for variability in the search
        std::random_shuffle(neighbors.begin(), neighbors.end());
        for(size_t i = 0; i < neighbors.size(); ++i)
        {
            // only add neighbors we have not visited
            if (!closedList_[neighbors[i].first])
            {
                OpenListNode nbr(neighbors[i].first);
                nbr.g = node.g + neighbors[i].second;
                nbr.h = weight * decomposition_->distanceHeuristic(neighbors[i].first, r2);
                nbr.parent = node.id;

                openList.push(nbr);
            }
        }
    }

    if (solution)
    {
        path.clear();
        int current = r2;
        while (predecessors_[current] != current)
        {
            path.insert(path.begin(), current);
            current = predecessors_[current];
        }

        path.insert(path.begin(), current); // add start state
    }

    return solution;
}

bool ompl::geometric::XXL::randomWalk(int r1, int r2, std::vector<int>& path)
{
    // Initialize predecessors and closed list
    std::fill(predecessors_.begin(), predecessors_.end(), -1);
    std::fill(closedList_.begin(), closedList_.end(), false);

    closedList_[r1] = true;
    for(int i = 0; i < decomposition_->getNumRegions(); ++i)
    {
        int u = i;
        // doing random walk until we hit a state already in the tree
        while (!closedList_[u])
        {
            std::vector<int> neighbors;
            decomposition_->getNeighbors(u, neighbors);
            int nbr = neighbors[rng_.uniformInt(0, neighbors.size()-1)]; // random successor

            predecessors_[u] = nbr;
            u = nbr;
        }

        // Adding the (simplified) random walk to the tree
        u = i;
        while (!closedList_[u])
        {
            closedList_[u] = true;
            u = predecessors_[u];
        }
    }

    int current = r2;
    path.clear();
    while (predecessors_[current] != -1)
    {
        path.insert(path.begin(), current);
        current = predecessors_[current];

        if ((int)path.size() >= decomposition_->getNumRegions())
            throw ompl::Exception("Serious problem in random walk");
    }
    path.insert(path.begin(), current); // add start state

    if (path.front() != r1)
        throw ompl::Exception("Path does not start at correct place");
    if (path.back() != r2)
        throw ompl::Exception("Path does not end at correct place");

    return true;
}