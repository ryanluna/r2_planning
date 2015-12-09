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

#ifndef OMPL_GEOMETRIC_PLANNERS_RLRT_RLRT_H_
#define OMPL_GEOMETRIC_PLANNERS_RLRT_RLRT_H_


#include "ompl/geometric/planners/PlannerIncludes.h"
#include <vector>

namespace ompl
{
    namespace geometric
    {
        /// \brief Range-Limited Random Tree (Ryan Luna's Random Tree)
        class RLRT : public base::Planner
        {
        public:

            RLRT(const base::SpaceInformationPtr &si);

            virtual ~RLRT();

            virtual void getPlannerData(base::PlannerData &data) const;

            virtual base::PlannerStatus solve(const base::PlannerTerminationCondition &ptc);

            virtual void clear();

            /// \brief Set the goal bias
            /// In the process of randomly selecting states in
            /// the state space to attempt to go towards, the
            /// algorithm may in fact choose the actual goal state, if
            /// it knows it, with some probability. This probability
            /// is a real number between 0.0 and 1.0; its value should
            /// usually be around 0.05 and should not be too large. It
            /// is probably a good idea to use the default value.
            void setGoalBias(double goalBias)
            {
                goalBias_ = goalBias;
            }

            /// \brief Get the goal bias the planner is using
            double getGoalBias() const
            {
                return goalBias_;
            }

            /// \brief Set the maximum distance between states in the tree.
            void setRange(double distance)
            {
                range_ = distance;
            }

            /// \brief Get the maximum distance between states in the tree.
            double getRange() const
            {
                return range_;
            }

            /// \brief If true, the planner will not have the range limitation.  Instead, if a collision is detected,
            /// the last valid state is retained.
            bool getKeepLast() const
            {
                return keepLast_;
            }

            /// \brief Set whether the planner will use the range or keep last heuristic.  If keepLast = false,
            /// motions are limited in distance to range_, otherwise the last valid state is retained when a collision
            /// is detected.
            void setKeepLast(bool keepLast)
            {
                keepLast_ = keepLast;
            }

            virtual void setup();

        protected:

            /// A motion (tree node) with parent pointer
            class Motion
            {
            public:

                Motion() : state(NULL), parent(NULL)
                {
                }

                /// \brief Constructor that allocates memory for the state
                Motion(const base::SpaceInformationPtr &si) : state(si->allocState()), parent(NULL)
                {
                }

                ~Motion()
                {
                }

                /** \brief The state contained by the motion */
                base::State       *state;

                /** \brief The parent motion in the exploration tree */
                Motion            *parent;

            };

            /** \brief Free the memory allocated by this planner */
            void freeMemory();


            /** \brief State sampler */
            base::StateSamplerPtr                          sampler_;

            /** \brief The fraction of time the goal is picked as the state to expand towards (if such a state is available) */
            double                                         goalBias_;

            /** \brief The maximum length of a motion to be added to a tree */
            double                                         range_;

            /// \brief If true, the planner will retain the last valid state during local planner.  Default is false.
            bool                                           keepLast_;

            /** \brief The random number generator */
            RNG                                            rng_;

            /** \brief The most recent goal motion.  Used for PlannerData computation */
            Motion                                         *lastGoalMotion_;

            std::vector<Motion*>                           motions_;
        };

    }
}

#endif