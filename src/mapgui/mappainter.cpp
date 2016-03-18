/*****************************************************************************
* Copyright 2015-2016 Alexander Barthel albar965@mailbox.org
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*****************************************************************************/

#include "mappainter.h"
#include "mapscale.h"
#include "common/mapcolors.h"
#include "geo/pos.h"
#include "geo/calculations.h"

#include <marble/GeoDataLineString.h>
#include <marble/GeoPainter.h>
#include <marble/MarbleWidget.h>
#include <marble/ViewportParams.h>

const int CIRCLE_MIN_POINTS = 16;
const int CIRCLE_MAX_POINTS = 72;

using namespace Marble;

MapPainter::MapPainter(Marble::MarbleWidget *marbleWidget, MapQuery *mapQuery, MapScale *mapScale,
                       bool verboseMsg)
  : CoordinateConverter(marbleWidget->viewport()), widget(marbleWidget), query(mapQuery), scale(mapScale),
    verbose(verboseMsg)
{
}

MapPainter::~MapPainter()
{
}

void MapPainter::setRenderHints(GeoPainter *painter)
{
  if(widget->viewContext() == Marble::Still)
  {
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::TextAntialiasing, true);
  }
  else if(widget->viewContext() == Marble::Animation)
  {
    painter->setRenderHint(QPainter::Antialiasing, false);
    painter->setRenderHint(QPainter::TextAntialiasing, false);
  }
}

void MapPainter::textBox(GeoPainter *painter, const QStringList& texts, const QPen& textPen, int x, int y,
                         textatt::TextAttributes atts, int transparency)
{
  if(texts.isEmpty())
    return;

  painter->save();

  if(transparency != 255)
  {
    if(transparency == 0)
      painter->setBrush(Qt::NoBrush);
    else
    {
      QColor col(mapcolors::textBoxColor);
      col.setAlpha(transparency);
      painter->setBrush(col);
    }
  }
  else
    painter->setBrush(mapcolors::textBoxColor);

  QFontMetrics metrics = painter->fontMetrics();
  int h = metrics.height();

  int yoffset = 0;
  if(transparency != 0)
  {
    painter->setPen(mapcolors::textBackgroundPen);
    for(const QString& t : texts)
    {
      int w = metrics.width(t);
      int newx = x - 2;
      // if(atts.testFlag(textatt::LEFT))
      // newx = x;
      if(atts.testFlag(textatt::RIGHT))
        newx -= w;
      else if(atts.testFlag(textatt::CENTER))
        newx -= w / 2;

      // painter->drawRoundedRect(x - 2, y - h + metrics.descent() + yoffset, w + 4, h, 5, 5);
      painter->drawRect(newx, y - h + metrics.descent() + yoffset, w + 4, h);
      yoffset += h;
    }
  }

  if(atts.testFlag(textatt::ITALIC) || atts.testFlag(textatt::BOLD) || atts.testFlag(textatt::UNDERLINE))
  {
    QFont f = painter->font();
    f.setBold(atts.testFlag(textatt::BOLD));
    f.setItalic(atts.testFlag(textatt::ITALIC));
    f.setUnderline(atts.testFlag(textatt::UNDERLINE));
    painter->setFont(f);
  }

  yoffset = 0;
  painter->setPen(textPen);
  for(const QString& t : texts)
  {
    int w = metrics.width(t);
    int newx = x;
    // if(atts.testFlag(textatt::LEFT))
    // newx = x;
    if(atts.testFlag(textatt::RIGHT))
      newx -= w;
    else if(atts.testFlag(textatt::CENTER))
      newx -= w / 2;

    painter->drawText(newx, y + yoffset, t);
    yoffset += h;
  }
  painter->restore();
}

void MapPainter::paintCircle(GeoPainter *painter, atools::geo::Pos pos, int radiusNm, bool fast,
                             int& xtext, int& ytext)
{
  // Calculate the number of points to use depending in screen resolution
  int pixel = scale->getPixelIntForMeter(atools::geo::nmToMeter(radiusNm));
  int numPoints = std::min(std::max(pixel / (fast ? 20 : 2), CIRCLE_MIN_POINTS), CIRCLE_MAX_POINTS);

  int radiusMeter = atools::geo::nmToMeter(radiusNm);

  int step = 360 / numPoints;
  int x1, y1, x2 = -1, y2 = -1, xfirst = -1, yfirst = -1;
  xtext = -1;
  ytext = -1;

  QVector<int> xtexts;
  QVector<int> ytexts;

  atools::geo::Pos p1 = pos.endpoint(radiusMeter, 0);
  bool h1 = true, h2 = true, hfirst = true;
  bool v1 = wToS(p1, x1, y1, &h1);

  // Remember first position to close the circle
  xfirst = x1;
  yfirst = y1;
  hfirst = h1;

  for(int i = 0; i <= 360; i += step)
  {
    atools::geo::Pos p2 = pos.endpoint(radiusMeter, i);
    bool v2 = wToS(p2, x2, y2, &h2);

    if((v1 || v2) && !h1 && !h2)
    {
      painter->drawLine(x1, y1, x2, y2);
      if(v1 && v2)
      {
        // Remember visible positions for the text
        xtexts.append((x1 + x2) / 2);
        ytexts.append((y1 + y2) / 2);
      }
    }
    x1 = x2;
    y1 = y2;
    v1 = v2;
    h1 = h2;
  }

  if(x2 != -1 && y2 != -1 && xfirst != -1 && yfirst != -1 && !h2 && !hfirst)
    painter->drawLine(x2, y2, xfirst, yfirst);

  if(!xtexts.isEmpty() && !ytexts.isEmpty())
  {
    // Take the position at one third
    xtext = xtexts.at(xtexts.size() / 3);
    ytext = ytexts.at(ytexts.size() / 3);
  }
  else
  {
    xtext = -1;
    ytext = -1;
  }
}

bool MapPainter::findTextPos(const GeoDataLineString& line, GeoPainter *painter, int w, int h,
                             int& x, int& y)
{
  GeoDataCoordinates center = line.first().interpolate(line.last(), 0.5);
  bool visible = wToS(center, x, y);
  if(visible && painter->window().contains(QRect(x - w / 2, y - h / 2, w, h)))
    return true;
  else
    // Check for 20 positions along the line starting below and above the center position
    for(double i = 0.; i < 0.5; i += 0.05)
    {
      center = line.first().interpolate(line.last(), 0.5 - i);
      visible = wToS(center, x, y);
      if(visible && painter->window().contains(QRect(x - w / 2, y - h / 2, w, h)))
        return true;

      center = line.first().interpolate(line.last(), 0.5 + i);
      visible = wToS(center, x, y);
      if(visible && painter->window().contains(QRect(x - w / 2, y - h / 2, w, h)))
        return true;
    }
  return false;
}