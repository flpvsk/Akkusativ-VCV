#include "plugin.hpp"

#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <boost/bind.hpp>

#include <oscpp/client.hpp>

#define MICROS_PER_SEC         1000000
#define us2s(x) (((double)x)/(double)MICROS_PER_SEC)

uint64_t getCurrentTime() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (
    ((uint64_t) tv.tv_sec + 2208988800L) << 32 |
    ((uint32_t) (us2s(tv.tv_usec) * (double)4294967296L))
  );
}


size_t makePacket(void* buffer, size_t size, std::string address, float number) {
  OSCPP::Client::Packet packet(buffer, size);
  packet
    .openBundle(getCurrentTime())
      .openMessage(address.c_str(), 1)
        .float32(number)
      .closeMessage()
      .openMessage("/u_speed", 1)
        .float32(0.2f)
      .closeMessage()
    .closeBundle();
  return packet.size();
}

const size_t kMaxPacketSize = 8192;

struct CVtoOSC : Module {
  rack::dsp::TTimer<float> timer;
  float lastReset = 0.f;

  std::array<char, kMaxPacketSize> buffer1;

  std::string url;
  bool isUrlDirty = false;

  std::string address1;
  bool isAddress1Dirty = false;

	enum ParamId {
    SAMPLE_RATE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		CV1_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUTPUTS_LEN
	};
	enum LightId {
		LIGHTS_LEN
	};

	CVtoOSC() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
    configParam(SAMPLE_RATE_PARAM, 0.00001f, 10.f, 0.001f, "Sample Rate", "s");
		configInput(CV1_INPUT, "CV1");
	}

  void onReset() override {
    timer.reset();

    url = "";
    isUrlDirty = true;

    address1 = "";
    isAddress1Dirty = true;
  }

  void fromJson(json_t* rootJ) override {
    Module::fromJson(rootJ);
    json_t* urlJ = json_object_get(rootJ, "url");
    if (urlJ) url = json_string_value(urlJ);
    isUrlDirty = true;

    json_t* address1J = json_object_get(rootJ, "address1");
    if (address1J) address1 = json_string_value(address1J);
    isAddress1Dirty = true;
  }

  json_t* dataToJson() override {
    json_t* rootJ = json_object();
    json_object_set_new(rootJ, "url", json_stringn(url.c_str(), url.size()));
    json_object_set_new(
      rootJ,
      "address1",
      json_stringn(address1.c_str(), address1.size())
    );
    return rootJ;
  }

  void dataFromJson(json_t* rootJ) override {
    json_t* urlJ = json_object_get(rootJ, "url");
    if (urlJ) url = json_string_value(urlJ);
    isUrlDirty = true;

    json_t* address1J = json_object_get(rootJ, "address1");
    if (address1J) address1 = json_string_value(address1J);
    isAddress1Dirty = true;
  }

	void process(const ProcessArgs& args) override {
    float cv1 = clamp(
        inputs[CV1_INPUT].getVoltage(),
        0.f,
        12.f
    ) / 12.f;
    float sampleRateParam = params[SAMPLE_RATE_PARAM].getValue();

    timer.process(args.sampleTime);
    if (timer.getTime() < sampleRateParam) {
      // DEBUG("t %f, lr %f, sr %f", timer.getTime(), lastReset, sampleRateParam);
      return;
    }

    timer.reset();

    using boost::asio::ip::udp;
    using boost::asio::ip::address;

    boost::asio::io_service io_service;
    boost::system::error_code err;

    udp::socket socket(io_service);
    udp::endpoint remoteEndpoint = udp::endpoint(
      address::from_string("127.0.0.1"), 7500
    );
    socket.open(udp::v4());

    size_t size = makePacket(&buffer1, kMaxPacketSize, address1, cv1);

    // DEBUG("sending %f to %s", cv1, address1.c_str());
    socket.send_to(
      boost::asio::buffer(buffer1, size),
      remoteEndpoint,
      0,
      err
    );

    socket.close();

    if (err.value() == boost::system::errc::success) {
      return;
    }

    DEBUG("error sending message %s", err.message().c_str());
	}
};

struct URLTextField: LedDisplayTextField {
  CVtoOSC* module;

  void step() override {
    LedDisplayTextField::step();
    if (!module || !module->isUrlDirty) return;
    setText(module->url);
    module->isUrlDirty = false;
  }

  void onChange(const ChangeEvent& e) override {
    if (module) module->url = getText();
  }
};


struct URLDisplay: LedDisplay {
  void setModule(CVtoOSC* module) {
    URLTextField* textField = createWidget<URLTextField>(Vec(0, 0));
    textField->box.size = box.size;
    textField->multiline = false;
    textField->module = module;
    addChild(textField);
  }
};

struct AddressTextField: LedDisplayTextField {
  CVtoOSC* module;

  void step() override {
    LedDisplayTextField::step();
    if (!module || !module->isAddress1Dirty) return;
    setText(module->address1);
    module->isAddress1Dirty = false;
  }

  void onChange(const ChangeEvent& e) override {
    if (module) module->address1 = getText();
  }
};


struct AddressDisplay: LedDisplay {
  void setModule(CVtoOSC* module) {
    AddressTextField* textField = createWidget<AddressTextField>(Vec(0, 0));
    textField->box.size = box.size;
    textField->multiline = false;
    textField->module = module;
    addChild(textField);
  }
};


struct CVtoOSCWidget : ModuleWidget {
	CVtoOSCWidget(CVtoOSC* module) {
		setModule(module);
		setPanel(
      createPanel(asset::plugin(pluginInstance, "res/Akkusativ_CV_OSC.svg"))
    );

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

    URLDisplay* urlDisplay = createWidget<URLDisplay>(Vec(0.0, 32));
    urlDisplay->box.size = Vec(150, 32);
    urlDisplay->setModule(module);
    addChild(urlDisplay);

    AddressDisplay* address1Display = createWidget<AddressDisplay>(Vec(0.0, 76));
    address1Display->box.size = Vec(150, 32);
    address1Display->setModule(module);
    addChild(address1Display);

		addInput(createInputCentered<PJ301MPort>(Vec(RACK_GRID_WIDTH, 120), module, CVtoOSC::CV1_INPUT));

    addParam(createParam<Trimpot>(Vec(RACK_GRID_WIDTH + 64, 120), module, CVtoOSC::SAMPLE_RATE_PARAM));
	}
};


Model* modelCVtoOSC = createModel<CVtoOSC, CVtoOSCWidget>("CVtoOSC");
