/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright 2011 Pierre Ossman <ossman@cendio.se> for Cendio AB
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <FL/fl_draw.H>

#include <rfb/CMsgWriter.h>
#include <rfb/LogWriter.h>

// FLTK can pull in the X11 headers on some systems
#ifndef XK_VoidSymbol
#define XK_MISCELLANY
#include <rfb/keysymdef.h>
#endif

#include "DesktopWindow.h"
#include "CConn.h"
#include "i18n.h"
#include "parameters.h"
#include "keysym2ucs.h"

using namespace rfb;

extern void exit_vncviewer();

static rfb::LogWriter vlog("DesktopWindow");

DesktopWindow::DesktopWindow(int w, int h, const char *name,
                             const rfb::PixelFormat& serverPF,
                             CConn* cc_)
  : Fl_Window(w, h), cc(cc_), frameBuffer(NULL), pixelTrans(NULL),
    lastPointerPos(0, 0), lastButtonMask(0)
{
  callback(handleClose, this);

  setName(name);

  frameBuffer = new ManagedPixelBuffer(getPreferredPF(), w, h);
  assert(frameBuffer);

  setServerPF(serverPF);

  show();
}


DesktopWindow::~DesktopWindow()
{
  // Unregister all timeouts in case they get a change tro trigger
  // again later when this object is already gone.
  Fl::remove_timeout(handleUpdateTimeout, this);
  Fl::remove_timeout(handleColourMap, this);
  Fl::remove_timeout(handlePointerTimeout, this);

  delete frameBuffer;

  if (pixelTrans)
    delete pixelTrans;
}


void DesktopWindow::setServerPF(const rfb::PixelFormat& pf)
{
  if (pixelTrans)
    delete pixelTrans;
  pixelTrans = NULL;

  if (pf.equal(getPreferredPF()))
    return;

  pixelTrans = new PixelTransformer();
  pixelTrans->init(pf, &colourMap, getPreferredPF());
}


const rfb::PixelFormat &DesktopWindow::getPreferredPF()
{
  static PixelFormat prefPF(32, 24, false, true, 255, 255, 255, 0, 8, 16);

  return prefPF;
}


// Cursor stuff

void DesktopWindow::setCursor(int width, int height, const Point& hotspot,
                              void* data, void* mask)
{
}


void DesktopWindow::setName(const char *name)
{
  CharArray windowNameStr;
  windowNameStr.replaceBuf(new char[256]);

  snprintf(windowNameStr.buf, 256, _("TigerVNC: %.240s"), name);

  copy_label(windowNameStr.buf);
}

// setColourMapEntries() changes some of the entries in the colourmap.
// Unfortunately these messages are often sent one at a time, so we delay the
// settings taking effect by 100ms.  This is because recalculating the internal
// translation table can be expensive.
void DesktopWindow::setColourMapEntries(int firstColour, int nColours,
                                        rdr::U16* rgbs)
{
  for (int i = 0; i < nColours; i++)
    colourMap.set(firstColour+i, rgbs[i*3], rgbs[i*3+1], rgbs[i*3+2]);

  if (!Fl::has_timeout(handleColourMap, this))
    Fl::add_timeout(0.100, handleColourMap, this);
}


// Copy the areas of the framebuffer that have been changed (damaged)
// to the displayed window.

void DesktopWindow::updateWindow()
{
  Rect r;

  Fl::remove_timeout(handleUpdateTimeout, this);

  r = damage.get_bounding_rect();
  Fl_Window::damage(FL_DAMAGE_USER1, r.tl.x, r.tl.y, r.width(), r.height());

  damage.clear();
}


void DesktopWindow::draw()
{
  int X, Y, W, H;

  int pixel_bytes, stride_bytes;
  const uchar *buf_start;

  // Check what actually needs updating
  fl_clip_box(0, 0, w(), h(), X, Y, W, H);
  if ((W == 0) || (H == 0))
    return;

  pixel_bytes = frameBuffer->getPF().bpp/8;
  stride_bytes = pixel_bytes * frameBuffer->getStride();
  buf_start = frameBuffer->data +
              pixel_bytes * X +
              stride_bytes * Y;

  // FIXME: Check how efficient this thing really is
  fl_draw_image(buf_start, X, Y, W, H, pixel_bytes, stride_bytes);
}


int DesktopWindow::handle(int event)
{
  int buttonMask, wheelMask;

  switch (event) {
  case FL_PUSH:
  case FL_RELEASE:
  case FL_DRAG:
  case FL_MOVE:
  case FL_MOUSEWHEEL:
    buttonMask = 0;
    if (Fl::event_button1())
      buttonMask |= 1;
    if (Fl::event_button2())
      buttonMask |= 2;
    if (Fl::event_button3())
      buttonMask |= 4;

    if (event == FL_MOUSEWHEEL) {
      if (Fl::event_dy() < 0)
        wheelMask = 8;
      else
        wheelMask = 16;

      // A quick press of the wheel "button", followed by a immediate
      // release below
      handlePointerEvent(Point(Fl::event_x(), Fl::event_y()),
                         buttonMask | wheelMask);
    } 

    handlePointerEvent(Point(Fl::event_x(), Fl::event_y()), buttonMask);
    return 1;

  case FL_FOCUS:
    // Yes, we would like some focus please!
    return 1;

  case FL_KEYDOWN:
    handleKeyEvent(Fl::event_key(), Fl::event_text(), true);
    return 1;

  case FL_KEYUP:
    handleKeyEvent(Fl::event_key(), Fl::event_text(), false);
    return 1;
  }

  return Fl_Window::handle(event);
}


void DesktopWindow::handleUpdateTimeout(void *data)
{
  DesktopWindow *self = (DesktopWindow *)data;

  assert(self);

  self->updateWindow();
}


void DesktopWindow::handleColourMap(void *data)
{
  DesktopWindow *self = (DesktopWindow *)data;

  assert(self);

  if (self->pixelTrans != NULL)
    self->pixelTrans->setColourMapEntries(0, 0);

  self->Fl_Window::damage(FL_DAMAGE_ALL);
}

void DesktopWindow::handleClose(Fl_Widget *wnd, void *data)
{
  exit_vncviewer();
}


void DesktopWindow::handlePointerEvent(const rfb::Point& pos, int buttonMask)
{
  if (!viewOnly) {
    if (pointerEventInterval == 0 || buttonMask != lastButtonMask) {
      cc->writer()->pointerEvent(pos, buttonMask);
    } else {
      if (!Fl::has_timeout(handlePointerTimeout, this))
        Fl::add_timeout((double)pointerEventInterval/1000.0,
                        handlePointerTimeout, this);
    }
    lastPointerPos = pos;
    lastButtonMask = buttonMask;
  }
}


void DesktopWindow::handlePointerTimeout(void *data)
{
  DesktopWindow *self = (DesktopWindow *)data;

  assert(self);

  self->cc->writer()->pointerEvent(self->lastPointerPos, self->lastButtonMask);
}


rdr::U32 DesktopWindow::translateKeyEvent(int keyCode, const char *keyText)
{
  unsigned ucs;

  // First check for function keys
  if ((keyCode >= FL_F) && (keyCode <= FL_F_Last))
    return XK_F1 + (keyCode - FL_F);

  // Then other special keys
  switch (keyCode) {
  case FL_BackSpace:
    return XK_BackSpace;
  case FL_Tab:
    return XK_Tab;
  case FL_Enter:
    return XK_Return;
  case FL_Pause:
    return XK_Pause;
  case FL_Scroll_Lock:
    return XK_Scroll_Lock;
  case FL_Escape:
    return XK_Escape;
  case FL_Home:
    return XK_Home;
  case FL_Left:
    return XK_Left;
  case FL_Up:
    return XK_Up;
  case FL_Right:
    return XK_Right;
  case FL_Down:
    return XK_Down;
  case FL_Page_Up:
    return XK_Page_Up;
  case FL_Page_Down:
    return XK_Page_Down;
  case FL_End:
    return XK_End;
  case FL_Print:
    return XK_Print;
  case FL_Insert:
    return XK_Insert;
  case FL_Menu:
    return XK_Menu;
  case FL_Help:
    return XK_Help;
  case FL_Num_Lock:
    return XK_Num_Lock;
  case FL_Shift_L:
    return XK_Shift_L;
  case FL_Shift_R:
    return XK_Shift_R;
  case FL_Control_L:
    return XK_Control_L;
  case FL_Control_R:
    return XK_Control_R;
  case FL_Caps_Lock:
    return XK_Caps_Lock;
  case FL_Meta_L:
    return XK_Super_L;
  case FL_Meta_R:
    return XK_Super_R;
  case FL_Alt_L:
    return XK_Alt_L;
  case FL_Alt_R:
    return XK_Alt_R;
  case FL_Delete:
    return XK_Delete;
  }

  // Unknown special key?
  if (keyText[0] == '\0') {
    vlog.error(_("Unknown FLTK key code %d (0x%04x)"), keyCode, keyCode);
    return XK_VoidSymbol;
  }

  // Look up the symbol the key produces and translate that from Unicode
  // to a X11 keysym.
  if (fl_utf_nb_char((const unsigned char*)keyText, strlen(keyText)) != 1) {
    vlog.error(_("Multiple characters given for key code %d (0x%04x): '%s'"),
               keyCode, keyCode, keyText);
    return XK_VoidSymbol;
  }

  ucs = fl_utf8decode(keyText, NULL, NULL);
  return ucs2keysym(ucs);
}


void DesktopWindow::handleKeyEvent(int keyCode, const char *keyText, bool down)
{
  rdr::U32 keySym;

  if (viewOnly)
    return;

  if (keyCode > 0xFFFF) {
      vlog.error(_("Too large FLTK key code %d (0x%08x)"), keyCode, keyCode);
      return;
  }

  // Because of the way keyboards work, we cannot expect to have the same
  // symbol on release as when pressed. This breaks the VNC protocol however,
  // so we need to keep track of what keysym a key _code_ generated on press
  // and send the same on release.
  if (!down) {
    cc->writer()->keyEvent(downKeySym[keyCode], false);
    return;
  }

  keySym = translateKeyEvent(keyCode, keyText);
  if (keySym == XK_VoidSymbol)
    return;

  downKeySym[keyCode] = keySym;
  cc->writer()->keyEvent(keySym, down);
}
