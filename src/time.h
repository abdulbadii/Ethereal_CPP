/*
  Ethereal is a UCI chess playing engine authored by Andrew Grant.
  <https://github.com/AndyGrant/Ethereal>     <andrew@grantnet.us>

  Ethereal is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Ethereal is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#if defined(_WIN32) || defined(_WIN64)
    #include <windows.h>
#else
    #include <sys/time.h>
#endif

#include "types.h"

double getRealTime();
double elapsedTime(SearchInfo *info);
void initTimeManagment(SearchInfo& info, const Limits& limits);
void updateTimeManagment(SearchInfo *info, Limits *limits);
int terminateTimeManagment(SearchInfo *info);
int terminateSearchEarly(Thread *thread);
namespace{
const double PVFactorCount  = 8;
const double PVFactorWeight = 0.085;}
