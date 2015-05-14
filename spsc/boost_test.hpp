/// @file boost_test.hpp
/// @brief Contains correct inclusion of Boost.Test lib (header only mode)
/// @copyright Licensed under the MIT License
/// @author Rostislav Ostapenko (rostislav.ostapenko@gmail.com)
/// @date 6-Apr-2014

#pragma once

#ifdef _MSC_VER
#ifdef NDEBUG
#pragma warning(push)
#pragma warning(disable : 4702)
#endif
#endif

#include <boost/test/included/unit_test.hpp>

#ifdef _MSC_VER
#ifdef NDEBUG
#pragma warning(pop)
#endif
#endif
