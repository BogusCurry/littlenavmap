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

#include "search/searchbase.h"
#include "gui/tablezoomhandler.h"
#include "search/sqlcontroller.h"
#include "gui/mainwindow.h"
#include "search/column.h"
#include "ui_mainwindow.h"
#include "search/columnlist.h"
#include "mapgui/mapwidget.h"
#include "atools.h"
#include "gui/actiontextsaver.h"
#include "export/csvexporter.h"
#include "mapgui/mapquery.h"
#include "options/optiondata.h"

#include <QTimer>
#include <QClipboard>

/* When using distance search delay the update the table after 500 milliseconds */
const int DISTANCE_EDIT_UPDATE_TIMEOUT_MS = 500;

SearchBase::SearchBase(MainWindow *parent, QTableView *tableView, ColumnList *columnList,
                       MapQuery *mapQuery, int tabWidgetIndex)
  : QObject(parent), columns(columnList), view(tableView), mainWindow(parent), query(mapQuery),
    tabIndex(tabWidgetIndex)
{
  zoomHandler = new atools::gui::TableZoomHandler(view);

  Ui::MainWindow *ui = mainWindow->getUi();

  // Avoid stealing of Ctrl-C from other default menus
  ui->actionSearchTableCopy->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionSearchResetSearch->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionSearchShowAll->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionSearchShowInformation->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  ui->actionSearchShowOnMap->setShortcutContext(Qt::WidgetWithChildrenShortcut);

  // Need extra action connected to catch the default Ctrl-C in the table view
  connect(ui->actionSearchTableCopy, &QAction::triggered, this, &SearchBase::tableCopyClipboard);

  // Actions that cover the whole dock window
  ui->dockWidgetSearch->addActions({ui->actionSearchResetSearch, ui->actionSearchShowAll});

  tableView->addActions({ui->actionSearchTableCopy, ui->actionSearchShowInformation,
                         ui->actionSearchShowOnMap});

  // Update single shot timer
  updateTimer = new QTimer(this);
  updateTimer->setSingleShot(true);
  connect(updateTimer, &QTimer::timeout, this, &SearchBase::editTimeout);

  connect(ui->actionSearchShowInformation, &QAction::triggered, this, &SearchBase::showInformationTriggered);
  connect(ui->actionSearchShowOnMap, &QAction::triggered, this, &SearchBase::showOnMapTriggered);

  // Load text size from options
  zoomHandler->zoomPercent(OptionData::instance().getGuiSearchTableTextSize());

}

SearchBase::~SearchBase()
{
  delete csvExporter;
  delete updateTimer;
  delete zoomHandler;
  delete columns;
}

/* Copy the selected rows of the table view as CSV into clipboard */
void SearchBase::tableCopyClipboard()
{
  if(view->isVisible())
  {
    QString csv;
    int exported = CsvExporter::selectionAsCsv(view, true, csv);

    if(!csv.isEmpty())
      QApplication::clipboard()->setText(csv);

    mainWindow->setStatusMessage(QString(tr("Copied %1 entries to clipboard.")).arg(exported));
  }
}

void SearchBase::initViewAndController()
{
  view->horizontalHeader()->setSectionsMovable(true);
  view->verticalHeader()->setSectionsMovable(false);
  view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);

  controller = new SqlController(mainWindow, mainWindow->getDatabase(), columns, view);
  controller->prepareModel();

  csvExporter = new CsvExporter(mainWindow, controller);
}

void SearchBase::filterByIdent(const QString& ident, const QString& region, const QString& airportIdent)
{
  controller->filterByIdent(ident, region, airportIdent);
}

void SearchBase::optionsChanged()
{
  // Need to reset model for "treat empty icons special"
  preDatabaseLoad();
  postDatabaseLoad();

  // Adapt table view text size
  zoomHandler->zoomPercent(OptionData::instance().getGuiSearchTableTextSize());

  view->update();
}

void SearchBase::updateTableSelection()
{
  tableSelectionChanged();
}

void SearchBase::searchMarkChanged(const atools::geo::Pos& mark)
{
  qDebug() << "new mark" << mark;
  if(columns->getDistanceCheckBox()->isChecked() && mark.isValid())
  {
    // Currently running distance search - update result
    QSpinBox *minDistanceWidget = columns->getMinDistanceWidget();
    QSpinBox *maxDistanceWidget = columns->getMaxDistanceWidget();
    QComboBox *distanceDirWidget = columns->getDistanceDirectionWidget();

    controller->filterByDistance(mark,
                                 static_cast<sqlproxymodel::SearchDirection>(distanceDirWidget->currentIndex()),
                                 minDistanceWidget->value(), maxDistanceWidget->value());

    controller->loadAllRowsForDistanceSearch();
  }
}

void SearchBase::connectSearchWidgets()
{
  void (QComboBox::*curIndexChangedPtr)(int) = &QComboBox::currentIndexChanged;
  void (QSpinBox::*valueChangedPtr)(int) = &QSpinBox::valueChanged;

  // Connect all column assigned widgets to lambda
  for(const Column *col : columns->getColumns())
  {
    if(col->getLineEditWidget() != nullptr)
    {
      connect(col->getLineEditWidget(), &QLineEdit::textChanged, [ = ](const QString &text)
              {
                controller->filterByLineEdit(col, text);
                updateButtonMenu();
                editStartTimer();
              });
    }
    else if(col->getComboBoxWidget() != nullptr)
    {
      connect(col->getComboBoxWidget(), curIndexChangedPtr, [ = ](int index)
              {
                controller->filterByComboBox(col, index, index == 0);
                updateButtonMenu();
                editStartTimer();
              });
    }
    else if(col->getCheckBoxWidget() != nullptr)
    {
      connect(col->getCheckBoxWidget(), &QCheckBox::stateChanged, [ = ](int state)
              {
                controller->filterByCheckbox(col, state, col->getCheckBoxWidget()->isTristate());
                updateButtonMenu();
                editStartTimer();
              });
    }
    else if(col->getSpinBoxWidget() != nullptr)
    {
      connect(col->getSpinBoxWidget(), valueChangedPtr, [ = ](int value)
              {
                controller->filterBySpinBox(col, value);
                updateButtonMenu();
                editStartTimer();
              });
    }
    else if(col->getMinSpinBoxWidget() != nullptr && col->getMaxSpinBoxWidget() != nullptr)
    {
      connect(col->getMinSpinBoxWidget(), valueChangedPtr, [ = ](int value)
              {
                controller->filterByMinMaxSpinBox(col, value, col->getMaxSpinBoxWidget()->value());
                col->getMaxSpinBoxWidget()->setMinimum(value);
                updateButtonMenu();
                editStartTimer();
              });

      connect(col->getMaxSpinBoxWidget(), valueChangedPtr, [ = ](int value)
              {
                controller->filterByMinMaxSpinBox(col, col->getMinSpinBoxWidget()->value(), value);
                col->getMinSpinBoxWidget()->setMaximum(value);
                updateButtonMenu();
                editStartTimer();
              });
    }
  }

  QSpinBox *minDistanceWidget = columns->getMinDistanceWidget();
  QSpinBox *maxDistanceWidget = columns->getMaxDistanceWidget();
  QComboBox *distanceDirWidget = columns->getDistanceDirectionWidget();
  QCheckBox *distanceCheckBox = columns->getDistanceCheckBox();

  if(minDistanceWidget != nullptr && maxDistanceWidget != nullptr &&
     distanceDirWidget != nullptr && distanceCheckBox != nullptr)
  {
    // If all distance widgets are present connect them
    connect(distanceCheckBox, &QCheckBox::stateChanged, this, &SearchBase::distanceSearchStateChanged);

    connect(minDistanceWidget, valueChangedPtr, [ = ](int value)
            {
              controller->filterByDistanceUpdate(
                static_cast<sqlproxymodel::SearchDirection>(distanceDirWidget->currentIndex()),
                value, maxDistanceWidget->value());
              maxDistanceWidget->setMinimum(value > 10 ? value : 10);
              updateButtonMenu();
              editStartTimer();
            });

    connect(maxDistanceWidget, valueChangedPtr, [ = ](int value)
            {
              controller->filterByDistanceUpdate(
                static_cast<sqlproxymodel::SearchDirection>(distanceDirWidget->currentIndex()),
                minDistanceWidget->value(), value);
              minDistanceWidget->setMaximum(value);
              updateButtonMenu();
              editStartTimer();
            });

    connect(distanceDirWidget, curIndexChangedPtr, [ = ](int index)
            {
              controller->filterByDistanceUpdate(static_cast<sqlproxymodel::SearchDirection>(index),
                                                 minDistanceWidget->value(), maxDistanceWidget->value());
              updateButtonMenu();
              editStartTimer();
            });
  }
}

void SearchBase::distanceSearchStateChanged(int state)
{
  distanceSearchChanged(state == Qt::Checked, true);
}

void SearchBase::distanceSearchChanged(bool checked, bool changeViewState)
{
  QSpinBox *minDistanceWidget = columns->getMinDistanceWidget();
  QSpinBox *maxDistanceWidget = columns->getMaxDistanceWidget();
  QComboBox *distanceDirWidget = columns->getDistanceDirectionWidget();

  if(changeViewState)
    saveViewState(!checked);

  controller->filterByDistance(
    checked ? mainWindow->getMapWidget()->getSearchMarkPos() : atools::geo::Pos(),
    static_cast<sqlproxymodel::SearchDirection>(distanceDirWidget->currentIndex()),
    minDistanceWidget->value(), maxDistanceWidget->value());

  minDistanceWidget->setEnabled(checked);
  maxDistanceWidget->setEnabled(checked);
  distanceDirWidget->setEnabled(checked);
  if(checked)
    controller->loadAllRowsForDistanceSearch();
  restoreViewState(checked);
  updateButtonMenu();
}

/* Search criteria editing has started. Start or restart the timer for a
 * delayed update if distance search is used */
void SearchBase::editStartTimer()
{
  if(controller->isDistanceSearch())
  {
    qDebug() << "editStarted";
    updateTimer->start(DISTANCE_EDIT_UPDATE_TIMEOUT_MS);
  }
}

/* Delayed update timeout. Update result if distance search is active */
void SearchBase::editTimeout()
{
  qDebug() << "editTimeout";
  controller->loadAllRowsForDistanceSearch();
}

void SearchBase::connectSearchSlots()
{
  connect(view, &QTableView::doubleClicked, this, &SearchBase::doubleClick);
  connect(view, &QTableView::customContextMenuRequested, this, &SearchBase::contextMenu);

  Ui::MainWindow *ui = mainWindow->getUi();

  connect(ui->actionSearchShowAll, &QAction::triggered, this, &SearchBase::loadAllRowsIntoView);
  connect(ui->actionSearchResetSearch, &QAction::triggered, this, &SearchBase::resetSearch);

  reconnectSelectionModel();

  connect(controller->getSqlModel(), &SqlModel::modelReset, this, &SearchBase::reconnectSelectionModel);
  void (SearchBase::*selChangedPtr)() = &SearchBase::tableSelectionChanged;
  connect(controller->getSqlModel(), &SqlModel::fetchedMore, this, selChangedPtr);

  connect(ui->dockWidgetSearch, &QDockWidget::visibilityChanged, this, &SearchBase::dockVisibilityChanged);
}

/* Connect selection model again after a SQL model reset */
void SearchBase::reconnectSelectionModel()
{
  if(view->selectionModel() != nullptr)
  {
    void (SearchBase::*selChangedPtr)(const QItemSelection &selected, const QItemSelection &deselected) =
      &SearchBase::tableSelectionChanged;

    connect(view->selectionModel(), &QItemSelectionModel::selectionChanged, this, selChangedPtr);
  }
}

/* Slot for table selection changed */
void SearchBase::tableSelectionChanged(const QItemSelection& selected, const QItemSelection& deselected)
{
  Q_UNUSED(selected);
  Q_UNUSED(deselected);

  tableSelectionChanged();
}

/* Update highlights if dock is hidden or shown (does not change for dock tab stacks) */
void SearchBase::dockVisibilityChanged(bool visible)
{
  Q_UNUSED(visible);

  tableSelectionChanged();
}

void SearchBase::tableSelectionChanged()
{
  QItemSelectionModel *sm = view->selectionModel();

  int selectedRows = 0;
  if(sm != nullptr && sm->hasSelection())
    selectedRows = sm->selectedRows().size();

  emit selectionChanged(this, selectedRows, controller->getVisibleRowCount(), controller->getTotalRowCount());
}

void SearchBase::preDatabaseLoad()
{
  saveViewState(controller->isDistanceSearch());
  controller->preDatabaseLoad();
}

void SearchBase::postDatabaseLoad()
{
  controller->postDatabaseLoad();
  restoreViewState(controller->isDistanceSearch());
}

/* Reset view sort order, column width and column order back to default values */
void SearchBase::resetView()
{
  Ui::MainWindow *ui = mainWindow->getUi();
  if(ui->tabWidgetSearch->currentIndex() == tabIndex)
  {
    controller->resetView();
    mainWindow->setStatusMessage(tr("Table view reset to defaults."));
  }
}

void SearchBase::resetSearch()
{
  Ui::MainWindow *ui = mainWindow->getUi();
  if(ui->tabWidgetSearch->currentIndex() == tabIndex)
  {
    controller->resetSearch();
    mainWindow->setStatusMessage(tr("Search filters cleared."));
  }
}

/* Loads all rows into the table view */
void SearchBase::loadAllRowsIntoView()
{
  Ui::MainWindow *ui = mainWindow->getUi();
  if(ui->tabWidgetSearch->currentIndex() == tabIndex)
  {
    controller->loadAllRows();
    mainWindow->setStatusMessage(tr("All entries read."));
  }
}

/* Double click into table view */
void SearchBase::doubleClick(const QModelIndex& index)
{
  if(index.isValid())
  {
    int row = index.row();

    // Check if the used table has bounding rectangle columns
    bool hasBounding = columns->hasColumn("left_lonx") && columns->hasColumn("top_laty") &&
                       columns->hasColumn("right_lonx") && columns->hasColumn("bottom_laty");

    // Show on map
    if(hasBounding)
    {
      float leftLon = controller->getRawData(row, "left_lonx").toFloat();
      float topLat = controller->getRawData(row, "top_laty").toFloat();
      float rightLon = controller->getRawData(row, "right_lonx").toFloat();
      float bottomLat = controller->getRawData(row, "bottom_laty").toFloat();
      emit showRect(atools::geo::Rect(leftLon, topLat, rightLon, bottomLat));
    }
    else
    {
      atools::geo::Pos p(controller->getRawData(row, "lonx").toFloat(),
                         controller->getRawData(row, "laty").toFloat());
      emit showPos(p, -1);
    }

    // Show on information panel
    maptypes::MapObjectTypes navType = maptypes::NONE;
    int id = -1;
    // get airport, VOR, NDB or waypoint id from model row
    getNavTypeAndId(row, navType, id);

    maptypes::MapSearchResult result;
    query->getMapObjectById(result, navType, id);

    emit showInformation(result);
  }
}

/* Context menu in table view selected */
void SearchBase::contextMenu(const QPoint& pos)
{
  Ui::MainWindow *ui = mainWindow->getUi();

  QPoint menuPos = QCursor::pos();
  // Use widget center if position is not inside widget
  if(!view->rect().contains(view->mapFromGlobal(QCursor::pos())))
    menuPos = view->mapToGlobal(view->rect().center());

  QString fieldData = "Data";

  // Save and restore action texts on return
  atools::gui::ActionTextSaver saver({ui->actionSearchFilterIncluding, ui->actionSearchFilterExcluding,
                                      ui->actionRouteAirportDest, ui->actionRouteAirportStart,
                                      ui->actionRouteAdd, ui->actionMapNavaidRange});
  Q_UNUSED(saver);

  bool columnCanFilter = false;
  atools::geo::Pos position;
  QModelIndex index = controller->getModelIndexAt(pos);
  maptypes::MapObjectTypes navType = maptypes::NONE;
  int id = -1;
  if(index.isValid())
  {
    const Column *columnDescriptor = columns->getColumn(index.column());
    Q_ASSERT(columnDescriptor != nullptr);
    columnCanFilter = columnDescriptor->isFilter();

    if(columnCanFilter)
      // Disabled menu items don't need any content
      fieldData = controller->getFieldDataAt(index);

    // Get position to display range rings
    position = atools::geo::Pos(controller->getRawData(index.row(), "lonx").toFloat(),
                                controller->getRawData(index.row(), "laty").toFloat());

    // get airport, VOR, NDB or waypoint id from model row
    getNavTypeAndId(index.row(), navType, id);
  }
  else
    qDebug() << "Invalid index at" << pos;

  // Add data to menu item text
  ui->actionSearchFilterIncluding->setText(ui->actionSearchFilterIncluding->text().
                                           arg("\"" + fieldData + "\""));
  ui->actionSearchFilterIncluding->setEnabled(index.isValid() && columnCanFilter);

  ui->actionSearchFilterExcluding->setText(ui->actionSearchFilterExcluding->text().
                                           arg("\"" + fieldData + "\""));
  ui->actionSearchFilterExcluding->setEnabled(index.isValid() && columnCanFilter);

  ui->actionMapNavaidRange->setEnabled(navType == maptypes::VOR || navType == maptypes::NDB);

  ui->actionRouteAdd->setEnabled(navType == maptypes::VOR || navType == maptypes::NDB ||
                                 navType == maptypes::WAYPOINT || navType == maptypes::AIRPORT);

  ui->actionRouteAirportDest->setEnabled(navType == maptypes::AIRPORT);
  ui->actionRouteAirportStart->setEnabled(navType == maptypes::AIRPORT);

  ui->actionMapRangeRings->setEnabled(index.isValid());
  ui->actionMapHideRangeRings->setEnabled(!mainWindow->getMapWidget()->getDistanceMarkers().isEmpty() ||
                                          !mainWindow->getMapWidget()->getRangeRings().isEmpty());

  ui->actionSearchSetMark->setEnabled(index.isValid());

  ui->actionMapNavaidRange->setText(tr("Show Navaid Range"));
  ui->actionRouteAdd->setText(tr("Add to Flight Plan"));
  ui->actionRouteAirportStart->setText(tr("Set as Flight Plan Departure"));
  ui->actionRouteAirportDest->setText(tr("Set as Flight Plan Destination"));

  ui->actionSearchTableCopy->setEnabled(index.isValid());
  ui->actionSearchTableSelectAll->setEnabled(controller->getTotalRowCount() > 0);

  // Build the menu
  QMenu menu;
  menu.addAction(ui->actionSearchShowInformation);
  menu.addAction(ui->actionSearchShowOnMap);
  menu.addSeparator();

  menu.addAction(ui->actionSearchFilterIncluding);
  menu.addAction(ui->actionSearchFilterExcluding);
  menu.addSeparator();

  menu.addAction(ui->actionSearchResetSearch);
  menu.addAction(ui->actionSearchShowAll);
  menu.addSeparator();

  menu.addAction(ui->actionMapRangeRings);
  menu.addAction(ui->actionMapNavaidRange);
  menu.addAction(ui->actionMapHideRangeRings);
  menu.addSeparator();

  menu.addAction(ui->actionRouteAirportStart);
  menu.addAction(ui->actionRouteAirportDest);
  menu.addSeparator();

  menu.addAction(ui->actionRouteAdd);
  menu.addSeparator();

  menu.addAction(ui->actionSearchTableCopy);
  menu.addAction(ui->actionSearchTableSelectAll);
  menu.addSeparator();

  menu.addAction(ui->actionSearchResetView);
  menu.addSeparator();

  menu.addAction(ui->actionSearchSetMark);

  QAction *action = menu.exec(menuPos);
  if(action != nullptr)
  {
    // A menu item was selected
    // Other actions with shortcuts are connected directly to methods/signals
    if(action == ui->actionSearchResetView)
      resetView();
    else if(action == ui->actionSearchTableCopy)
      tableCopyClipboard();
    else if(action == ui->actionSearchFilterIncluding)
      controller->filterIncluding(index);
    else if(action == ui->actionSearchFilterExcluding)
      controller->filterExcluding(index);
    else if(action == ui->actionSearchTableSelectAll)
      controller->selectAllRows();
    else if(action == ui->actionSearchSetMark)
      emit changeSearchMark(controller->getGeoPos(index));
    else if(action == ui->actionMapRangeRings)
      mainWindow->getMapWidget()->addRangeRing(position);
    else if(action == ui->actionMapNavaidRange)
    {
      // Radio navaid range ring
      int frequency = controller->getRawData(index.row(), "frequency").toInt();
      if(navType == maptypes::VOR)
        // Adapt scaled frequency from nav_search table to the scale used the VOR table
        frequency /= 10;

      mainWindow->getMapWidget()->addNavRangeRing(position, navType,
                                                  controller->getRawData(index.row(), "ident").toString(),
                                                  frequency,
                                                  controller->getRawData(index.row(), "range").toInt());
    }
    else if(action == ui->actionMapHideRangeRings)
      mainWindow->getMapWidget()->clearRangeRingsAndDistanceMarkers();
    else if(action == ui->actionRouteAdd)
      emit routeAdd(id, atools::geo::EMPTY_POS, navType, -1);
    else if(action == ui->actionRouteAirportStart)
    {
      maptypes::MapAirport ap;
      query->getAirportById(ap, controller->getIdForRow(index));
      emit routeSetDeparture(ap);
    }
    else if(action == ui->actionRouteAirportDest)
    {
      maptypes::MapAirport ap;
      query->getAirportById(ap, controller->getIdForRow(index));
      emit routeSetDestination(ap);
    }
  }
}

/* Triggered by show information action in context menu. Populates map search result and emits show information */
void SearchBase::showInformationTriggered()
{
  Ui::MainWindow *ui = mainWindow->getUi();
  if(ui->tabWidgetSearch->currentIndex() == tabIndex)
  {
    // Index covers a cell
    QModelIndex index = view->currentIndex();
    if(index.isValid())
    {
      maptypes::MapObjectTypes navType = maptypes::NONE;
      int id = -1;
      getNavTypeAndId(index.row(), navType, id);

      maptypes::MapSearchResult result;
      query->getMapObjectById(result, navType, id);
      emit showInformation(result);
    }
  }
}

/* Show on map action in context menu */
void SearchBase::showOnMapTriggered()
{
  Ui::MainWindow *ui = mainWindow->getUi();
  if(ui->tabWidgetSearch->currentIndex() == tabIndex)
  {
    QModelIndex index = view->currentIndex();
    if(index.isValid())
    {
      maptypes::MapObjectTypes navType = maptypes::NONE;
      int id = -1;
      getNavTypeAndId(index.row(), navType, id);

      maptypes::MapSearchResult result;
      query->getMapObjectById(result, navType, id);

      if(!result.airports.isEmpty())
      {
        emit showRect(result.airports.first().bounding);
        mainWindow->setStatusMessage(tr("Showing airport on map."));
      }
      else
      {
        if(!result.vors.isEmpty())
          emit showPos(result.vors.first().getPosition(), -1);
        else if(!result.ndbs.isEmpty())
          emit showPos(result.ndbs.first().getPosition(), -1);
        else if(!result.waypoints.isEmpty())
          emit showPos(result.waypoints.first().getPosition(), -1);
        mainWindow->setStatusMessage(tr("Showing navaid on map."));
      }
    }
  }
}

/* Fetch nav type and database id from a model row */
void SearchBase::getNavTypeAndId(int row, maptypes::MapObjectTypes& navType, int& id)
{
  navType = maptypes::NONE;
  id = -1;

  if(columns->getTablename() == "airport")
  {
    // Airport table
    navType = maptypes::AIRPORT;
    id = controller->getRawData(row, columns->getIdColumn()->getIndex()).toInt();
  }
  else
  {
    // Otherwise nav_search table
    navType = maptypes::navTypeToMapObjectType(controller->getRawData(row, "nav_type").toString());

    if(navType == maptypes::VOR)
      id = controller->getRawData(row, "vor_id").toInt();
    else if(navType == maptypes::NDB)
      id = controller->getRawData(row, "ndb_id").toInt();
    else if(navType == maptypes::WAYPOINT)
      id = controller->getRawData(row, "waypoint_id").toInt();
  }
}
