// Copyright (C) 2003 Free Software Foundation, Inc.
//
// This file is part of the GNU ISO C++ Library.  This library is free
// software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the
// Free Software Foundation; either version 2, or (at your option)
// any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License along
// with this library; see the file COPYING.  If not, write to the Free
// Software Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307,
// USA.

// 27.8.1.4 Overridden virtual functions

#include <fstream>
#include <testsuite_hooks.h>

void test03()
{
  using namespace std;

  bool test __attribute__((unused)) = true;
  const char* name = "tmp_seekoff_3";

  wfilebuf fb;

  fb.open(name, ios_base::out);
  fb.sputc(0xf001);

  // seekoff should flush the output sequence, which will fail
  // if the output buffer contains illegal characters.
  streampos ret = fb.pubseekoff(0, ios_base::cur);
  VERIFY( ret == streampos(streamoff(-1)) );
}

int main()
{
  test03();
  return 0;
}
