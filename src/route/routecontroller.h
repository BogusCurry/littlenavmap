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

#ifndef LITTLENAVMAP_ROUTECONTROLLER_H
#define LITTLENAVMAP_ROUTECONTROLLER_H

#include "route/routecommand.h"
#include "route/routemapobjectlist.h"
#include "common/maptypes.h"

#include <QObject>

namespace atools {
namespace gui {
class TableZoomHandler;
}

namespace fs {
namespace pln {
class Flightplan;
class FlightplanEntry;
}
}
}

class MainWindow;
class QTableView;
class QStandardItemModel;
class QItemSelection;
class RouteIconDelegate;
class RouteNetwork;
class RouteFinder;
class FlightplanEntryBuilder;

/*
 * All flight plan related tasks like saving, loading, modification, calculation and table
 * view display are managed in this class.
 *
 * Flight plan and route map objects are maintained in parallel to keep flight plan structure
 * similar to the loaded original (i.e. waypoints not in database, missing airways)
 */
class RouteController :
  public QObject
{
  Q_OBJECT

public:
  RouteController(MainWindow *parent, MapQuery *mapQuery, QTableView *tableView);
  virtual ~RouteController();

  /* Creates a new plan and emits routeChanged */
  void newFlightplan();

  /* Loads flight plan from FSX PLN file, checks for proper start position (shows notification dialog)
   * and emits routeChanged. Uses file name as new current name  */
  bool loadFlightplan(const QString& filename);
  void loadFlightplan(const atools::fs::pln::Flightplan& flightplan,
                      const QString& filename = QString(), bool quiet = false);

  /* Loads flight plan from FSX PLN file and appends it to the current flight plan.
   * Emits routeChanged. */
  bool appendFlightplan(const QString& filename);

  /* Saves flight plan using the given name and uses file name as new current name */
  bool saveFlighplanAs(const QString& filename);

  /* Saves flight plan using current name */
  bool saveFlightplan();

  /* Save and reload widgets state and current flight plan name */
  void saveState();
  void restoreState();

  const RouteMapObjectList& getRouteMapObjects() const
  {
    return route;
  }

  /* Get a copy of all route map objects (legs) that are selected in the flight plan table view */
  void getSelectedRouteMapObjects(QList<int>& selRouteMapObjectIndexes) const;

  /* Get bounding rectangle for flight plan */
  const atools::geo::Rect& getBoundingRect() const
  {
    return boundingRect;
  }

  /* Has flight plan changed */
  bool hasChanged() const;

  /* Get the current flight plan name or empty if no plan is loaded */
  const QString& getCurrentRouteFilename() const
  {
    return routeFilename;
  }

  /* Create a default filename based on departure and destination names */
  QString buildDefaultFilename() const;

  /* @return true if no flight plan loaded (no departure, no destination and no waypoints) */
  bool isFlightplanEmpty() const;

  /* @return true if flight plan is not empty and first entry (departure) is an airport */
  bool hasValidDeparture() const;

  /* @return true if flight plan is not empty and last entry (destination) is an airport */
  bool hasValidDestination() const;

  /* @return true if flight plan has intermediate waypoints */
  bool hasEntries() const;

  /* @return true if flight plan has at least two waypoints */
  bool canCalcRoute() const;

  /* @return true if departure is valid and departure airport has no parking or departure of flight plan
   *  has parking or helipad as start position */
  bool hasValidParking() const;

  /* Clear routing network cache and disconnect all queries */
  void preDatabaseLoad();
  void postDatabaseLoad();

  /* Replaces departure airport or adds departure if not valid. Adds best start position (runway). */
  void routeSetDeparture(maptypes::MapAirport airport);

  /* Replaces destination airport or adds destination if not valid */
  void routeSetDestination(maptypes::MapAirport airport);

  /*
   * Adds a navaid, airport or user defined position to flight plan.
   * @param id Id of object to insert
   * @param userPos Coordinates of user defined position if no navaid is to be inserted.
   * @param type Type of object to insert. maptypes::USER if userPos is set.
   * @param legIndex Insert after the leg with this index. Will use nearest leg if index is -1.
   */
  void routeAdd(int id, atools::geo::Pos userPos, maptypes::MapObjectTypes type, int legIndex);

  /* Same as above but replaces waypoint at legIndex */
  void routeReplace(int id, atools::geo::Pos userPos, maptypes::MapObjectTypes type, int legIndex);

  /* Delete waypoint at the given index. Will also delete departure or destination */
  void routeDelete(int index);

  /* Set departure parking position. If the airport of the parking spot is different to
   * the current departure it will be replaced too. */
  void routeSetParking(maptypes::MapParking parking);

  /* Shows the dialog to select departure parking or start position.
   *  @return true if position was set. false is dialog was canceled. */
  bool selectDepartureParking();

  /* "Calculate" a direct flight plan that has no waypoints. */
  void calculateDirect();

  /* Calculate a flight plan from radio navaid to radio navaid */
  void calculateRadionav();

  /* Calculate a flight plan along high altitude (Jet) airways */
  void calculateHighAlt();

  /* Calculate a flight plan along low altitude (Victor) airways */
  void calculateLowAlt();

  /* Calculate a flight plan along low and high altitude airways that have the given altitude from
   *  the spin box as minimum altitude */
  void calculateSetAlt();

  /* Reverse order of all waypoints, swap departure and destination and automatically
   * select a new start position (best runway) */
  void reverse();

  void optionsChanged();

  /* Get the route table as a HTML document only containing the table and header */
  QString tableAsHtml(int iconSizePixel) const;

  /* Copy the route as a string to the clipboard */
  void routeStringToClipboard() const;

signals:
  /* Show airport on map */
  void showRect(const atools::geo::Rect& rect);

  /* Show flight plan waypoint or user position on map */
  void showPos(const atools::geo::Pos& pos, int zoom);

  /* Change distance search center */
  void changeMark(const atools::geo::Pos& pos);

  /* Selection in table view has changed. Update hightlights on map */
  void routeSelectionChanged(int selected, int total);

  /* Route has changed */
  void routeChanged(bool geometryChanged);

  /* Show information about the airports or navaids in the search result */
  void showInformation(maptypes::MapSearchResult result);

  /* Emitted before route calculation to stop any background tasks */
  void preRouteCalc();

private:
  friend class RouteCommand;

  /* Move selected rows */
  enum MoveDirection
  {
    MOVE_NONE = 0,
    MOVE_DOWN = 1,
    MOVE_UP = -1
  };

  /* Called by route command */
  void changeRouteUndo(const atools::fs::pln::Flightplan& newFlightplan);

  /* Called by route command */
  void changeRouteRedo(const atools::fs::pln::Flightplan& newFlightplan);

  /* Called by route command */
  void undoMerge();

  RouteCommand *preChange(const QString& text = QString(), rctype::RouteCmdType rcType = rctype::EDIT);
  void postChange(RouteCommand *undoCommand);

  void routeSetStartPosition(maptypes::MapStart start);

  void updateWindowLabel();

  void doubleClick(const QModelIndex& index);
  void tableContextMenu(const QPoint& pos);

  void tableSelectionChanged(const QItemSelection& selected, const QItemSelection& deselected);

  void moveSelectedLegsDown();
  void moveSelectedLegsUp();
  void moveSelectedLegsInternal(MoveDirection direction);
  void deleteSelectedLegs();
  void selectedRows(QList<int>& rows, bool reverse);

  void select(QList<int>& rows, int offset);

  void updateMoveAndDeleteActions();

  void routeToFlightPlan();

  void routeSetDepartureInternal(const maptypes::MapAirport& airport);

  void updateTableModel();

  void createRouteMapObjects();
  void updateRouteMapObjects();
  void updateBoundingRect();

  void routeAltChanged();
  void routeTypeChanged();

  void clearRoute();

  bool calculateRouteInternal(RouteFinder *routeFinder, atools::fs::pln::RouteType type,
                              const QString& commandName,
                              bool fetchAirways, bool useSetAltitude);

  void updateFlightplanEntryAirway(int airwayId, atools::fs::pln::FlightplanEntry& entry, int& minAltitude);

  void updateModelRouteTime();

  void updateFlightplanFromWidgets();

  /* Used by undo/redo */
  void changeRouteUndoRedo(const atools::fs::pln::Flightplan& newFlightplan);

  void tableCopyClipboard();

  void showInformationMenu();
  void showOnMapMenu();

  void undoTriggered();
  void redoTriggered();
  bool updateStartPositionBestRunway(bool force, bool undo);

  void dockVisibilityChanged(bool visible);
  void eraseAirway(int row);
  QString buildFlightplanLabel(bool html) const;
  QString buildFlightplanLabel2() const;

  /* If route distance / direct distance if bigger than this value fail routing */
  static Q_DECL_CONSTEXPR float MAX_DISTANCE_DIRECT_RATIO = 1.5f;

  static Q_DECL_CONSTEXPR int ROUTE_UNDO_LIMIT = 50;

  atools::gui::TableZoomHandler *zoomHandler = nullptr;

  /* Need a workaround since QUndoStack does not report current indices and clean state correctly */
  int undoIndex = 0;
  /* Clean index of the undo stack or -1 if not clean state exists */
  int undoIndexClean = 0;

  /* Used to number user defined positions */
  int curUserpointNumber = 1;

  /* Network cache for flight plan calculation */
  RouteNetwork *routeNetworkRadio = nullptr, *routeNetworkAirway = nullptr;
  atools::geo::Rect boundingRect;
  RouteMapObjectList route;
  /* Current filename of empty if no route */
  QString routeFilename;
  MainWindow *mainWindow;
  QTableView *view;
  MapQuery *query;
  QStandardItemModel *model;
  RouteIconDelegate *iconDelegate = nullptr;
  QUndoStack *undoStack = nullptr;
  FlightplanEntryBuilder *entryBuilder = nullptr;

};

#endif // LITTLENAVMAP_ROUTECONTROLLER_H
